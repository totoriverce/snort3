//--------------------------------------------------------------------------
// Copyright (C) 2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2016 Titan IC Systems. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// rxp.cc author Titan IC Systems <support@titanicsystems.com>

#include <assert.h>
#include <ctype.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <rte_config.h>
#include <rte_eal.h>
#include <rxp.h>

#include "framework/mpse.h"
#include "log/messages.h"
#include "main/snort_config.h"
#include "utils/stats.h"

using namespace std;

// Escape a pattern to a form suitable for feeding to the RXP compiler.
// Anything non-printable is represented as \x<value>. Caller must free
// returned string.
static string* rxp_escape_pattern(const uint8_t* pat, unsigned len)
{
    int i;
    string* escpat = nullptr;
    char hexbyte[5];

    if (len == 0)
        return nullptr;

    escpat = new string;

    for (i = 0; i < len; i++)
    {
        // Could be less strict; but 'if (pat[i] < 32 or pat[i] > 126)' is too loose
        if (!isalnum(pat[i]))
        {
            sprintf(hexbyte, "\\x%02x", pat[i]);
            escpat->append(hexbyte);
        }
        else
        {
            escpat->append(1, (const char) pat[i]);
        }
    }

    return escpat;
}

struct UserCtx {
    void *user;
    void *user_tree;
    void *user_list;

    UserCtx(void *u);
};

UserCtx::UserCtx(void *u)
{
    user = u;
    user_tree = user_list = nullptr;
}

struct RxpPattern
{
    std::string* pat;
    uint16_t ruleid;
    bool no_case;
    bool negate;

    vector<struct UserCtx> userctx;

    RxpPattern(string* pattern, const Mpse::PatternDescriptor& d, void *u);
    ~RxpPattern(void);
};

RxpPattern::RxpPattern(string* pattern, const Mpse::PatternDescriptor& d, void *u)
{
    pat = pattern;

    no_case = d.no_case;
    negate = d.negated;
    userctx.push_back(UserCtx(u));
}

RxpPattern::~RxpPattern(void)
{
    delete pat;
}

//-------------------------------------------------------------------------
// mpse
//-------------------------------------------------------------------------

class RxpMpse : public Mpse
{
public:
    RxpMpse(SnortConfig*, bool use_gc, const MpseAgent* a)
        : Mpse("rxp", use_gc)
    {
        agent = a;
        instances.push_back(this);
        instance_id = instances.size();
    }

    ~RxpMpse()
    {
        user_dtor();
    }

    int add_pattern(
        SnortConfig*, const uint8_t* pat, unsigned len,
        const PatternDescriptor& desc, void* user) override;

    int prep_patterns(SnortConfig*) override;

    int _search(const uint8_t*, int, MpseMatch, void*, int*) override;

    int get_pattern_count() override { return pats.size(); }

    int rxp_search(const uint8_t* buf, int n, MpseMatch mf, void *pv);

    static int write_rule_file(const string& filename);
    static int build_rule_file(const string& filename, const string& rulesdir);
    static int program_rule_file(const string& rulesdir);

    static int dpdk_init(void);

private:
    void user_ctor(SnortConfig*);
    void user_dtor();

    const MpseAgent* agent;

    map<int, RxpPattern*> ruleidtbl;    // Maps rule ids to pattern + user ctx.
    uint64_t instance_id;               // This is used as the RXP subset ID


public:
    vector<RxpPattern*> pats;

    static uint64_t duplicates;
    static uint64_t jobs_submitted;
    static uint64_t match_limit;
    static uint64_t patterns;
    static uint64_t max_pattern_len;
    static vector<RxpMpse*> instances;
    static unsigned portid;
};

uint64_t RxpMpse::duplicates = 0;
uint64_t RxpMpse::jobs_submitted = 0;
uint64_t RxpMpse::match_limit = 0;
uint64_t RxpMpse::patterns = 0;
uint64_t RxpMpse::max_pattern_len = 0;
vector<RxpMpse*> RxpMpse::instances;
unsigned RxpMpse::portid = 0;

// We don't have an accessible FSM match state, so like Hyperscan we build a simple
// tree for each option. However the same pattern can be used for several rules, so
// each RXP match may result in multiple rules we need to pass back to the snort core.
void RxpMpse::user_ctor(SnortConfig* sc)
{
    unsigned i;

    for ( auto& p : pats )
    {
        for ( auto& c : p->userctx )
        {
            if ( c.user )
            {
                if ( p->negate )
                    agent->negate_list(c.user, &c.user_list);
                else
                    agent->build_tree(sc, c.user, &c.user_tree);
            }
            agent->build_tree(sc, nullptr, &c.user_tree);
        }
    }
}

void RxpMpse::user_dtor()
{
    unsigned i;

    for ( auto& p : pats )
    {
        for ( auto& c : p->userctx )
        {
            if ( c.user )
                agent->user_free(c.user);

            if ( c.user_list )
                agent->list_free(&c.user_list);

            if ( c.user_tree )
                agent->tree_free(&c.user_tree);
        }
    }
}

int RxpMpse::add_pattern(SnortConfig*, const uint8_t* pat, unsigned len,
    const PatternDescriptor& desc, void* user)
{
    RxpPattern* rxp_pat = nullptr;
    string* pattern = rxp_escape_pattern(pat, len);

    for ( auto& p : pats)
    {
        if (*p->pat == *pattern) {
            rxp_pat = p;
            break;
        }
    }

    if (rxp_pat)
    {
        // It's a duplicate pattern, record it so we can report back multiple matches
        rxp_pat->userctx.push_back(UserCtx(user));
        ++duplicates;
    }
    else
    {
        rxp_pat = new RxpPattern(pattern, desc, user);

        rxp_pat->ruleid = ++patterns;
        ruleidtbl[rxp_pat->ruleid] = rxp_pat;

        if (len > max_pattern_len)
            max_pattern_len = len;

        pats.push_back(rxp_pat);
    }

    return 0;
}

int RxpMpse::prep_patterns(SnortConfig* sc)
{
    user_ctor(sc);
    return 0;
}

int RxpMpse::rxp_search(const uint8_t* buf, int n, MpseMatch mf, void *pv)
{
    struct rte_mbuf* job_buf;
    struct rte_mbuf* pkts_burst[32];
    struct rxp_response_data rxp_resp;
    int i, ret;
    unsigned sent, pending, rx_pkts;

    // FIXIT-T: Split job up and overlap; too big for RXP
    if (n > RXP_MAX_JOB_LENGTH)
    {
        LogMessage("WARNING: Truncating search from %d bytes to %d.\n", n, RXP_MAX_JOB_LENGTH);
        n = RXP_MAX_JOB_LENGTH;
    }

    // FIXIT-T: Only a single subset at once here.
    ret = rxp_prepare_job(portid, ++jobs_submitted /* Job ID can't be 0 */,
        (uint8_t *) buf, n, 0 /* ctrl */, instance_id, instance_id, instance_id, instance_id, &job_buf);

    ret = rxp_enqueue_job(portid, 0 /* queue id */, job_buf);

    ret = rxp_dispatch_jobs(portid, 0 /* queue id */, &sent, &pending);

    rx_pkts = 0;
    while (rx_pkts == 0)
    {
        ret = rxp_get_responses(portid, 0 /* queue id */, pkts_burst, 1, &rx_pkts);
    }

    ret = rxp_get_response_data(pkts_burst[0], &rxp_resp);

    if (ret)
        LogMessage("ERROR: %d decoding RXP response.\n", ret);

    if (rxp_resp.match_count != 0) {
        if (rxp_resp.detected_match_count > rxp_resp.match_count)
        {
            LogMessage("WARNING: Detected %u matches but only %u returned.\n",
                rxp_resp.detected_match_count, rxp_resp.match_count);
            match_limit++;
            // FIXIT-T: We should fall back to a software search engine here. For now keep going.
        }

        for (i = 0; i < rxp_resp.match_count; i++) {
            int to = rxp_resp.match_data[i].start_ptr + rxp_resp.match_data[i].length;
            RxpPattern *pat = ruleidtbl[rxp_resp.match_data[i].rule_id];

            for ( auto& c : pat->userctx )
            {
                mf(c.user, c.user_tree, to, pv, c.user_list);
            }
        }
    }

    rxp_free_buffer(pkts_burst[0]);

    return 0;
}

int RxpMpse::_search(
    const uint8_t* buf, int n, MpseMatch mf, void* pv, int* current_state)
{
    *current_state = 0;

    SnortState* ss = snort_conf->state + get_instance_id();

    rxp_search(buf, n, mf, pv);

    return 0;
}

// Functions relating to the generation of the RXP rules file

int RxpMpse::write_rule_file(const string& filename)
{
    ofstream rulesfile;
    unsigned int rule, subset;

    rulesfile.open(filename);

    rulesfile << "# TICS subsets file for Snort-3.0" << endl;

    for (subset = 0; subset < instances.size(); subset++)
    {
        rulesfile << "subset_id = " << subset + 1 << endl;

        for (vector<RxpPattern*>::iterator rule = instances[subset]->pats.begin();
            rule != instances[subset]->pats.end(); rule++)
        {
            rulesfile << (*rule)->ruleid << ", " << *(*rule)->pat << endl;
        }
    }

    rulesfile.close();

    return 0;
}

int RxpMpse::build_rule_file(const string& filename, const string& rulesdir)
{
    ostringstream rxpc_cmd_str;

    rxpc_cmd_str << "rxpc -f " << filename << " -o " << rulesdir << "/snort3 --ptpb 0 -F -i";

    return system(rxpc_cmd_str.str().c_str());
}

int RxpMpse::program_rule_file(const string& rulesdir)
{
    ostringstream rulesfile;

    rulesfile << rulesdir << "/snort3.rof";

    return rxp_program_rules_memories(portid, 0 /* queue id */, rulesfile.str().c_str());
}

int RxpMpse::dpdk_init(void)
{
    char *dpdk_argv[4];

    dpdk_argv[0] = strdup("snort");
    dpdk_argv[1] = strdup("-c");
    dpdk_argv[2] = strdup("1");
    dpdk_argv[3] = strdup("--");

    if (rte_eal_init(4, dpdk_argv) < 0)
    {
        LogMessage("ERROR: Failed to initialise DPDK EAL.\n");
        exit(-1);
    }

    if (rxp_port_init(portid, 1 /* num queues */, 1))
    {
        LogMessage("ERROR: Failed to initialise RXP port.\n");
        exit(-1);
    }

    if (rxp_init(portid))
    {
        LogMessage("ERROR: Failed to initialise RXP.\n");
        exit(-1);
    }

    return 0;
}

//-------------------------------------------------------------------------
// api
//-------------------------------------------------------------------------

static void rxp_setup(SnortConfig* sc)
{
    // FIXIT-T: These file paths should be a configuration setting.
    RxpMpse::write_rule_file("/tmp/snort3.rules");
    RxpMpse::build_rule_file("/tmp/snort3.rules", "/tmp/rules-dir");

    RxpMpse::dpdk_init();
    RxpMpse::program_rule_file("/tmp/rules-dir");
    rxp_enable(RxpMpse::portid);
}

static Mpse* rxp_ctor(
    SnortConfig* sc, class Module*, bool use_gc, const MpseAgent* a)
{
    RxpMpse* instance;

    instance = new RxpMpse(sc, use_gc, a);

    return instance;
}

static void rxp_dtor(Mpse* p)
{
    delete p;
}

static void rxp_init()
{
    RxpMpse::jobs_submitted = 0;
    RxpMpse::match_limit = 0;
    RxpMpse::patterns = 0;
    RxpMpse::max_pattern_len = 0;
}

static void rxp_print()
{
    LogCount("instances", RxpMpse::instances.size());
    LogCount("patterns", RxpMpse::patterns);
    LogCount("duplicate patterns", RxpMpse::duplicates);
    LogCount("maximum pattern length", RxpMpse::max_pattern_len);
    LogCount("RXP jobs submitted", RxpMpse::jobs_submitted);
    LogCount("RXP match limit exceeded", RxpMpse::match_limit);
}

static const MpseApi rxp_api =
{
    {
        PT_SEARCH_ENGINE,
        sizeof(MpseApi),
        SEAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        "rxp",
        "Titan IC Systems RXP-based hardware acclerated regex mpse",
        nullptr,
        nullptr
    },
    false,
    nullptr,
    rxp_setup,
    nullptr,  // start
    nullptr,  // stop
    rxp_ctor,
    rxp_dtor,
    rxp_init,
    rxp_print,
    nullptr,
    nullptr,
};

const BaseApi* se_rxp = &rxp_api.base;

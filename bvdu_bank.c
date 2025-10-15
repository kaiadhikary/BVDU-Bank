/* ============================================================================

   Project : 🏦 BVDU-Bank — Banking + Trading Management System
   Author  : Adarsh Satyajit Adhikary
   Year    : 2025
   License : MIT License (see LICENSE file)
   Admin PIN : 0013

   Description :
   A console-based Banking + Trading simulation written in C, featuring:
   - Account creation, UPI, loans, transfers, bill payments
   - Integrated trading system (Stocks / Crypto / Forex)
   - Random live price simulation across Indian, US, and EU markets
   - Admin dashboard with audit logs, notifications, and account management
   - File-based data persistence — no external database required

   Compile :
       gcc bvdu_bank.c -o bvdu_bank

   Run :
       ./bvdu_bank   (Linux/macOS)
       .\bvdu_bank.exe   (Windows)

   Notes :
   - Keep all .txt data files in the same directory as this source file.
   - Uses ANSI escape codes for colored profit/loss (enable ANSI in Windows terminal).
   - Educational project for demonstration and portfolio use.

   © 2025 Adarsh Satyajit Adhikary — BVDU-Bank
   All Rights Reserved.

============================================================================ */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* ---------------- Configuration ---------------- */
#define MAX_ACCOUNTS 500
#define MAX_HOLDINGS 2000
#define MAX_PRICES 200
#define MAX_LINE 512
#define MINI_STAT_LIMIT 10

/* Files */
static const char *F_ACCOUNTS = "accounts.txt";
static const char *F_TRANSACTIONS = "transactions.txt";
static const char *F_HOLDINGS = "holdings.txt";
static const char *F_PRICES = "prices.txt";
static const char *F_FX = "fx_rates.txt";
static const char *F_ADMIN_AUDIT = "admin_audit.txt";
static const char *F_NOTIFICATIONS = "notifications.txt";

/* Admin PIN */
static const int ADMIN_PIN = 0013;

/* ANSI color codes */
static const char *ANSI_RED = "\x1b[31m";
static const char *ANSI_GREEN = "\x1b[32m";
static const char *ANSI_RESET = "\x1b[0m";

/* ---------------- Data structures ---------------- */

typedef struct {
    int acc_no;
    char name[50];
    char acc_type[20]; /* Savings/Current */
    int pin;
    double balance;    /* cash in INR */
    double loan;
    int active;        /* 1 active 0 deleted */
    int frozen;        /* 1 locked after failed attempts */
    int failed_attempts;
    char upi[64];
    char last_login[25];
} Account;

typedef struct {
    int acc_no;
    char timestamp[25];
    char type[20];
    double amount;
    double balance_after;
    char note[80];
} Transaction;

/* Holding: which account owns which asset */
typedef struct {
    int acc_no;
    char asset_id[16];
    char asset_name[64];
    double qty;
    double avg_price;  /* in asset's native currency (e.g., USD for US market) */
    char market[8];    /* "IN","US","EU" */
} Holding;

/* Price record: asset, price in native currency, volatility, market, last updated */
typedef struct {
    char asset_id[16];
    char asset_name[64];
    double price;      /* price per unit in that market currency (USD, EUR, INR) */
    double vol;        /* volatility factor (0.01 = ~1%) */
    char market[8];    /* IN, US, EU */
    char last_update[25];
    int open_hour;     /* market open hour (0-23 local) */
    int close_hour;    /* market close hour (0-23 local) */
} PriceRec;

/* FX rates: INR per USD and INR per EUR */
typedef struct {
    double inr_per_usd;
    double inr_per_eur;
    char last_update[25];
} FXRates;

/* ---------------- In-memory arrays ---------------- */
static Account accounts[MAX_ACCOUNTS];
static int acc_count = 0;

static Holding holdings[MAX_HOLDINGS];
static int hold_count = 0;

static PriceRec prices[MAX_PRICES];
static int price_count = 0;

static FXRates fx = {83.5, 88.2, ""};

/* ---------------- Utility functions ---------------- */

static void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[n-1] = '\0'; n--; }
}

static void get_timestamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(buf, n, "%Y-%m-%d %H:%M:%S", tm);
    else strncpy(buf, "1970-01-01 00:00:00", n);
}

/* safe input readers (fgets + parse) */
static int safe_read_int(void) {
    char buf[128];
    if (!fgets(buf, sizeof buf, stdin)) return -1;
    trim_newline(buf);
    return atoi(buf);
}
static double safe_read_double(void) {
    char buf[128];
    if (!fgets(buf, sizeof buf, stdin)) return -1.0;
    trim_newline(buf);
    return atof(buf);
}

/* portable stricmp to avoid collision with platform libs */
static int bvdu_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

/* lowercase in-place */
static void strtolower_inplace(char *s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

/* atomic file write (write to tmp then rename) */
static int atomic_write_text(const char *filename, const char *tmpname, const char *content) {
    FILE *f = fopen(tmpname, "w");
    if (!f) return 0;
    fputs(content, f);
    fclose(f);
    remove(filename);
    if (rename(tmpname, filename) != 0) {
        /* fallback: try remove and rename */
        return 0;
    }
    return 1;
}

/* append line to text file */
static void append_line(const char *filename, const char *line) {
    FILE *f = fopen(filename, "a");
    if (!f) return;
    fputs(line, f);
    fputs("\n", f);
    fclose(f);
}

/* timestamped admin audit append */
static void audit_log(const char *entry) {
    char ts[25];
    get_timestamp(ts, sizeof ts);
    char buf[512];
    snprintf(buf, sizeof buf, "%s|%s", ts, entry);
    append_line(F_ADMIN_AUDIT, buf);
}

/* push notification */
static void push_notification(int acc_no, const char *msg) {
    char ts[25];
    get_timestamp(ts, sizeof ts);
    char buf[512];
    snprintf(buf, sizeof buf, "%s|%d|%s", ts, acc_no, msg);
    append_line(F_NOTIFICATIONS, buf);
}

/* ---------------- File load/save routines ---------------- */

static void save_accounts(void) {
    /* atomic save */
    const char *tmp = "accounts.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { perror("save_accounts fopen"); return; }
    for (int i = 0; i < acc_count; ++i) {
        Account *a = &accounts[i];
        /* acc_no|name|acc_type|pin|balance|loan|active|frozen|failed_attempts|upi|last_login */
        fprintf(f, "%d|%s|%s|%d|%.2f|%.2f|%d|%d|%d|%s|%s\n",
            a->acc_no, a->name, a->acc_type, a->pin, a->balance, a->loan,
            a->active, a->frozen, a->failed_attempts, a->upi, a->last_login);
    }
    fclose(f);
    remove(F_ACCOUNTS);
    rename(tmp, F_ACCOUNTS);
}

static void load_accounts(void) {
    FILE *f = fopen(F_ACCOUNTS, "r");
    if (!f) { acc_count = 0; return; }
    acc_count = 0;
    while (!feof(f) && acc_count < MAX_ACCOUNTS) {
        Account a;
        char upi[64] = "", last_login[25] = "";
        int r = fscanf(f, "%d|%49[^|]|%19[^|]|%d|%lf|%lf|%d|%d|%d|%63[^|]|%24[^\n]\n",
            &a.acc_no, a.name, a.acc_type, &a.pin, &a.balance, &a.loan,
            &a.active, &a.frozen, &a.failed_attempts, upi, last_login);
        if (r == 11) {
            strncpy(a.upi, upi, sizeof a.upi - 1);
            strncpy(a.last_login, last_login, sizeof a.last_login - 1);
            accounts[acc_count++] = a;
        } else break;
    }
    fclose(f);
}

static void append_transaction(const Transaction *t) {
    FILE *f = fopen(F_TRANSACTIONS, "a");
    if (!f) { perror("append_transaction"); return; }
    /* acc_no|timestamp|type|amount|balance_after|note */
    fprintf(f, "%d|%s|%s|%.2f|%.2f|%s\n",
        t->acc_no, t->timestamp, t->type, t->amount, t->balance_after, t->note);
    fclose(f);
}

/* holdings */
static void save_holdings(void) {
    FILE *f = fopen(F_HOLDINGS, "w");
    if (!f) { perror("save_holdings fopen"); return; }
    for (int i = 0; i < hold_count; ++i) {
        Holding *h = &holdings[i];
        /* acc_no|asset_id|asset_name|qty|avg_price|market */
        fprintf(f, "%d|%s|%s|%.6f|%.4f|%s\n", h->acc_no, h->asset_id, h->asset_name, h->qty, h->avg_price, h->market);
    }
    fclose(f);
}

static void load_holdings(void) {
    FILE *f = fopen(F_HOLDINGS, "r");
    if (!f) { hold_count = 0; return; }
    hold_count = 0;
    while (!feof(f) && hold_count < MAX_HOLDINGS) {
        Holding h;
        int r = fscanf(f, "%d|%15[^|]|%63[^|]|%lf|%lf|%7[^\n]\n",
            &h.acc_no, h.asset_id, h.asset_name, &h.qty, &h.avg_price, h.market);
        if (r == 6) holdings[hold_count++] = h;
        else break;
    }
    fclose(f);
}

/* prices (atomic) */
static void save_prices(void) {
    const char *tmp = "prices.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { perror("save_prices fopen"); return; }
    for (int i = 0; i < price_count; ++i) {
        PriceRec *p = &prices[i];
        fprintf(f, "%s|%s|%.4f|%.6f|%s|%s|%d|%d\n",
            p->asset_id, p->asset_name, p->price, p->vol, p->market, p->last_update, p->open_hour, p->close_hour);
    }
    fclose(f);
    remove(F_PRICES);
    rename(tmp, F_PRICES);
}

static void load_prices(void) {
    FILE *f = fopen(F_PRICES, "r");
    if (!f) { price_count = 0; return; }
    price_count = 0;
    while (!feof(f) && price_count < MAX_PRICES) {
        PriceRec p;
        int r = fscanf(f, "%15[^|]|%63[^|]|%lf|%lf|%7[^|]|%24[^|]|%d|%d\n",
            p.asset_id, p.asset_name, &p.price, &p.vol, p.market, p.last_update, &p.open_hour, &p.close_hour);
        if (r == 8) prices[price_count++] = p;
        else break;
    }
    fclose(f);
}

/* fx rates */
static void save_fx(void) {
    const char *tmp = "fx.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { perror("save_fx fopen"); return; }
    fprintf(f, "%.6f|%.6f|%s\n", fx.inr_per_usd, fx.inr_per_eur, fx.last_update);
    fclose(f);
    remove(F_FX);
    rename(tmp, F_FX);
}

static void load_fx(void) {
    FILE *f = fopen(F_FX, "r");
    if (!f) { /* defaults already set */ return; }
    char last[25] = {0};
    int r = fscanf(f, "%lf|%lf|%24[^\n]\n", &fx.inr_per_usd, &fx.inr_per_eur, last);
    if (r == 3) strncpy(fx.last_update, last, sizeof fx.last_update - 1);
    fclose(f);
}

/* ---------------- Helper finders ---------------- */

static int find_account_index(int acc_no) {
    for (int i = 0; i < acc_count; ++i) if (accounts[i].acc_no == acc_no) return i;
    return -1;
}

static int find_active_account_index(int acc_no) {
    for (int i = 0; i < acc_count; ++i) if (accounts[i].acc_no == acc_no && accounts[i].active) return i;
    return -1;
}

static int find_account_by_upi(const char *upi) {
    for (int i = 0; i < acc_count; ++i) if (accounts[i].active && bvdu_stricmp(accounts[i].upi, upi) == 0) return i;
    return -1;
}

static int is_upi_unique(const char *upi) {
    for (int i = 0; i < acc_count; ++i) if (bvdu_stricmp(accounts[i].upi, upi) == 0) return 0;
    return 1;
}

static int find_price_index(const char *asset_id) {
    for (int i = 0; i < price_count; ++i) if (strcmp(prices[i].asset_id, asset_id) == 0) return i;
    return -1;
}

static int find_holding_index(int acc_no, const char *asset_id) {
    for (int i = 0; i < hold_count; ++i)
        if (holdings[i].acc_no == acc_no && strcmp(holdings[i].asset_id, asset_id) == 0) return i;
    return -1;
}

/* ---------------- Transaction logging ---------------- */

static void log_transaction(int acc_no, const char *type, double amount, double balance_after, const char *note) {
    Transaction t;
    t.acc_no = acc_no;
    get_timestamp(t.timestamp, sizeof t.timestamp);
    strncpy(t.type, type, sizeof t.type - 1); t.type[sizeof t.type - 1] = '\0';
    t.amount = amount;
    t.balance_after = balance_after;
    strncpy(t.note, note, sizeof t.note - 1); t.note[sizeof t.note - 1] = '\0';
    append_transaction(&t);
}

/* ---------------- Utilities: market time & tick ---------------- */

static int market_is_open(const PriceRec *p) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int hour = tm->tm_hour;
    /* simple: compare hour in local timezone */
    if (p->open_hour <= p->close_hour) return (hour >= p->open_hour && hour < p->close_hour);
    /* wrap-around e.g., open 20 to 4 */
    return (hour >= p->open_hour || hour < p->close_hour);
}

/* small random double in [-1,1] */
static double rand_minus1_1(void) {
    return ((double)rand() / RAND_MAX) * 2.0 - 1.0;
}

/* single market tick: adjust prices for market that is open */
static void tick_market_once(void) {
    char ts[25];
    get_timestamp(ts, sizeof ts);
    for (int i = 0; i < price_count; ++i) {
        PriceRec *p = &prices[i];
        if (market_is_open(p)) {
            /* percent change ~ N(0, vol) scaled by random */
            double change_pct = rand_minus1_1() * p->vol;
            double old = p->price;
            p->price *= (1.0 + change_pct);
            if (p->price < 0.0001) p->price = 0.0001;
            strncpy(p->last_update, ts, sizeof p->last_update - 1);
        }
    }
    save_prices();
    char entry[128]; snprintf(entry, sizeof entry, "MARKET_TICK|ALL_MARKETS");
    audit_log(entry);
}

/* allow admin to randomize all prices (larger move) */
static void admin_randomize_all_prices(void) {
    srand((unsigned)time(NULL));
    for (int i = 0; i < price_count; ++i) {
        PriceRec *p = &prices[i];
        double change_pct = rand_minus1_1() * p->vol * 5.0; /* bigger */
        p->price *= (1.0 + change_pct);
        if (p->price < 0.0001) p->price = 0.0001;
        get_timestamp(p->last_update, sizeof p->last_update);
    }
    save_prices();
    audit_log("ADMIN_RANDOMIZE_PRICES");
}

/* ---------------- Portfolio valuation ---------------- */

/* Convert asset price in native currency to INR using fx rates */
static double price_in_inr(const PriceRec *p) {
    if (strcmp(p->market, "IN") == 0) return p->price;
    if (strcmp(p->market, "US") == 0) return p->price * fx.inr_per_usd;
    if (strcmp(p->market, "EU") == 0) return p->price * fx.inr_per_eur;
    return p->price; /* fallback */
}

/* compute portfolio value (in INR) for account */
static double compute_portfolio_value_inr(int acc_no) {
    double tot = 0.0;
    for (int i = 0; i < hold_count; ++i) {
        if (holdings[i].acc_no != acc_no) continue;
        int pidx = find_price_index(holdings[i].asset_id);
        double cur_price = (pidx >= 0) ? price_in_inr(&prices[pidx]) : holdings[i].avg_price * ((strcmp(holdings[i].market,"US")==0)?fx.inr_per_usd:1.0);
        tot += holdings[i].qty * cur_price;
    }
    return tot;
}

/* compute unrealized P/L in INR */
static double compute_unrealized_pl_inr(int acc_no) {
    double pl = 0.0;
    for (int i = 0; i < hold_count; ++i) {
        if (holdings[i].acc_no != acc_no) continue;
        int pidx = find_price_index(holdings[i].asset_id);
        double cur_price_native = (pidx >= 0) ? prices[pidx].price : holdings[i].avg_price;
        double cur_inr;
        if (strcmp(holdings[i].market, "IN") == 0) cur_inr = cur_price_native;
        else if (strcmp(holdings[i].market, "US") == 0) cur_inr = cur_price_native * fx.inr_per_usd;
        else if (strcmp(holdings[i].market, "EU") == 0) cur_inr = cur_price_native * fx.inr_per_eur;
        else cur_inr = cur_price_native;
        double avg_inr = holdings[i].avg_price * (strcmp(holdings[i].market,"US")==0 ? fx.inr_per_usd : (strcmp(holdings[i].market,"EU")==0 ? fx.inr_per_eur : 1.0));
        pl += holdings[i].qty * (cur_inr - avg_inr);
    }
    return pl;
}

/* ---------------- New helpers: account number & UPI validation ---------------- */

/* Return next account number: max acc_no + 1 or 1001 if none */
static int next_account_no(void) {
    int max = 1000;
    for (int i = 0; i < acc_count; ++i) if (accounts[i].acc_no > max) max = accounts[i].acc_no;
    return max + 1;
}

/* Validate UPI local part: only letters and digits, not empty.
   Accept these user inputs:
     - "alice" -> becomes "alice@bvdu"
     - "alice@bvdu" -> accepted
   Reject:
     - anything with additional '@' or domain not 'bvdu'
     - local part containing non-alnum characters or spaces
   Returns 1 and writes normalized lowercased upi into out_upi on success, 0 on failure.
*/
static int validate_and_normalize_upi(const char *input, char *out_upi, size_t out_len) {
    if (!input || !out_upi) return 0;
    char buf[128];
    strncpy(buf, input, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
    trim_newline(buf);
    if (strlen(buf) == 0) return 0;

    /* If contains '@', split once */
    char *at = strchr(buf, '@');
    if (at) {
        /* if more than one '@' => reject */
        if (strchr(at+1, '@') != NULL) return 0;
        *at = '\0';
        char *local = buf;
        char *domain = at + 1;
        strtolower_inplace(local); strtolower_inplace(domain);
        /* domain must be exactly "bvdu" */
        if (strcmp(domain, "bvdu") != 0) return 0;
        /* local must be alnum only */
        if (strlen(local) == 0) return 0;
        for (size_t i = 0; i < strlen(local); ++i) {
            if (!isalnum((unsigned char)local[i])) return 0;
        }
        snprintf(out_upi, out_len, "%s@bvdu", local);
        return 1;
    } else {
        /* no '@' -> local part only, ensure alnum */
        strtolower_inplace(buf);
        for (size_t i = 0; i < strlen(buf); ++i) {
            if (!isalnum((unsigned char)buf[i])) return 0;
        }
        snprintf(out_upi, out_len, "%s@bvdu", buf);
        return 1;
    }
}

/* ---------------- User actions: accounts ---------------- */

static void create_account_interactive(void) {
    if (acc_count >= MAX_ACCOUNTS) { printf("Account limit reached.\n"); return; }
    char buf[256];
    Account a;

    /* auto-assign account number */
    a.acc_no = next_account_no();

    printf("Creating account number: %d\n", a.acc_no);

    printf("Enter name (single word preferred): ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); strncpy(a.name, buf, sizeof a.name - 1); a.name[sizeof a.name - 1] = '\0';

    printf("Account type (Savings/Current) [Savings]: ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf);
    if (strlen(buf) == 0) strncpy(a.acc_type, "Savings", sizeof a.acc_type - 1);
    else strncpy(a.acc_type, buf, sizeof a.acc_type - 1);
    a.acc_type[sizeof a.acc_type - 1] = '\0';

    printf("Set 4-digit PIN: ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); a.pin = atoi(buf);
    if (a.pin < 1000 || a.pin > 9999) { printf("PIN must be 4-digit.\n"); return; }

    printf("Initial deposit amount (INR): ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); a.balance = atof(buf);

    /* UPI selection + validation */
    char candidate[128];
    printf("Choose UPI local part (letters/numbers only). Leave empty to use '%s': ", a.name);
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf);
    if (strlen(buf) == 0) {
        /* use name as local part */
        char tmp[80]; strncpy(tmp, a.name, sizeof tmp - 1); tmp[sizeof tmp - 1] = '\0';
        if (!validate_and_normalize_upi(tmp, candidate, sizeof candidate)) {
            /* fallback: use acc_no as local */
            snprintf(candidate, sizeof candidate, "%d@bvdu", a.acc_no);
        }
    } else {
        /* user provided something: validate */
        if (!validate_and_normalize_upi(buf, candidate, sizeof candidate)) {
            printf("Invalid UPI. Must be letters and/or numbers only, domain must be @bvdu or omitted.\n");
            return;
        }
    }
    /* ensure uniqueness */
    if (!is_upi_unique(candidate)) {
        printf("UPI '%s' already taken. Choose a unique UPI.\n", candidate);
        return;
    }
    strncpy(a.upi, candidate, sizeof a.upi - 1);
    a.upi[sizeof a.upi - 1] = '\0';

    a.loan = 0.0;
    a.active = 1;
    a.frozen = 0;
    a.failed_attempts = 0;
    get_timestamp(a.last_login, sizeof a.last_login);
    accounts[acc_count++] = a;
    save_accounts();

    char note[128]; snprintf(note, sizeof note, "Account created (UPI:%s)", a.upi);
    log_transaction(a.acc_no, "CREATE", a.balance, a.balance, note);
    char audit[256]; snprintf(audit, sizeof audit, "CREATE_ACCOUNT|%d|%s|%s", a.acc_no, a.name, a.upi);
    audit_log(audit);
    push_notification(a.acc_no, "Welcome! Account created.");
    printf("Account %d created with UPI '%s'.\n", a.acc_no, a.upi);
}

/* Authenticate - returns account index or -1.
   If PIN wrong increments failed_attempts and freezes after 3. */
static int authenticate_prompt(void) {
    char buf[128];
    printf("Enter account number: ");
    if (!fgets(buf, sizeof buf, stdin)) return -1;
    trim_newline(buf); int acc_no = atoi(buf);
    int idx = find_account_index(acc_no);
    if (idx == -1) { printf("Account not found.\n"); return -1; }
    if (!accounts[idx].active) { printf("Account inactive.\n"); return -1; }
    if (accounts[idx].frozen) { printf("Account frozen. Contact admin.\n"); return -1; }

    printf("Enter PIN: ");
    if (!fgets(buf, sizeof buf, stdin)) return -1;
    trim_newline(buf); int pin = atoi(buf);
    if (accounts[idx].pin == pin) {
        accounts[idx].failed_attempts = 0;
        get_timestamp(accounts[idx].last_login, sizeof accounts[idx].last_login);
        save_accounts();
        return idx;
    } else {
        accounts[idx].failed_attempts++;
        if (accounts[idx].failed_attempts >= 3) {
            accounts[idx].frozen = 1;
            save_accounts();
            audit_log("ACCOUNT_FROZEN");
            printf("Too many failed attempts. Account frozen. Admin must unfreeze.\n");
        } else {
            save_accounts();
            printf("Invalid PIN. Attempts left: %d\n", 3 - accounts[idx].failed_attempts);
        }
        return -1;
    }
}

/* Deposit and Withdraw (auth inside) */
static void deposit_money(void) {
    int idx = authenticate_prompt(); if (idx < 0) return;
    printf("Enter amount to deposit (INR): ");
    double amt = safe_read_double();
    if (amt <= 0) { printf("Invalid amount.\n"); return; }
    accounts[idx].balance += amt;
    save_accounts();
    log_transaction(accounts[idx].acc_no, "DEPOSIT", amt, accounts[idx].balance, "Deposit");
    push_notification(accounts[idx].acc_no, "Deposit successful.");
    printf("Deposit complete. New balance: %.2f INR\n", accounts[idx].balance);
}

static void withdraw_money(void) {
    int idx = authenticate_prompt(); if (idx < 0) return;
    printf("Enter amount to withdraw (INR): ");
    double amt = safe_read_double();
    if (amt <= 0) { printf("Invalid amount.\n"); return; }
    if (amt > accounts[idx].balance) { printf("Insufficient funds.\n"); return; }
    accounts[idx].balance -= amt;
    save_accounts();
    log_transaction(accounts[idx].acc_no, "WITHDRAW", -amt, accounts[idx].balance, "Withdraw");
    push_notification(accounts[idx].acc_no, "Withdrawal processed.");
    printf("Withdraw successful. New balance: %.2f INR\n", accounts[idx].balance);
}

/* Transfer when already logged in (does not re-authenticate) */
static void transfer_from_loggedin(int from_idx) {
    if (from_idx < 0 || from_idx >= acc_count) { printf("Internal error.\n"); return; }
    char buf[128];
    printf("Enter destination account number: ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); int to_acc = atoi(buf);
    int to_idx = find_active_account_index(to_acc);
    if (to_idx < 0) { printf("Destination not found or not active.\n"); return; }
    if (accounts[to_idx].frozen) { printf("Destination frozen. Cannot receive funds.\n"); return; }
    if (to_idx == from_idx) { printf("Cannot transfer to same account.\n"); return; }
    printf("Enter amount to transfer (INR): ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); double amt = atof(buf);
    if (amt <= 0) { printf("Invalid amount.\n"); return; }
    if (amt > accounts[from_idx].balance) { printf("Insufficient funds.\n"); return; }
    accounts[from_idx].balance -= amt;
    accounts[to_idx].balance += amt;
    save_accounts();
    char note1[80]; snprintf(note1, sizeof note1, "Transfer to %d", accounts[to_idx].acc_no);
    char note2[80]; snprintf(note2, sizeof note2, "Transfer from %d", accounts[from_idx].acc_no);
    log_transaction(accounts[from_idx].acc_no, "TRANSFER_OUT", -amt, accounts[from_idx].balance, note1);
    log_transaction(accounts[to_idx].acc_no, "TRANSFER_IN", amt, accounts[to_idx].balance, note2);
    push_notification(accounts[to_idx].acc_no, "You have received a transfer.");
    printf("Transfer successful. New balance: %.2f INR\n", accounts[from_idx].balance);
}

/* Transfer (auth inside) kept for convenience */
static void transfer_money(void) {
    int idx = authenticate_prompt();
    if (idx < 0) return;
    transfer_from_loggedin(idx);
}

/* UPI transfer (logged-in) - only to registered UPIs */
static void upi_transfer_from_loggedin(int from_idx) {
    if (from_idx < 0 || from_idx >= acc_count) { printf("Internal error.\n"); return; }
    char buf[128];
    printf("Enter destination UPI (e.g., alice@bvdu): ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); strtolower_inplace(buf);
    int to_idx = find_account_by_upi(buf);
    if (to_idx < 0) { printf("UPI not found. Transfers allowed only to registered BVDU UPIs.\n"); return; }
    if (!accounts[to_idx].active) { printf("Destination not active.\n"); return; }
    if (accounts[to_idx].frozen) { printf("Destination frozen.\n"); return; }
    if (to_idx == from_idx) { printf("Cannot send to own UPI.\n"); return; }
    printf("Enter amount (INR): ");
    double amt = safe_read_double();
    if (amt <= 0) { printf("Invalid.\n"); return; }
    if (amt > accounts[from_idx].balance) { printf("Insufficient funds.\n"); return; }
    accounts[from_idx].balance -= amt;
    accounts[to_idx].balance += amt;
    save_accounts();
    char note1[80]; snprintf(note1, sizeof note1, "UPI to %s", accounts[to_idx].upi);
    char note2[80]; snprintf(note2, sizeof note2, "UPI from %s", accounts[from_idx].upi);
    log_transaction(accounts[from_idx].acc_no, "UPI_OUT", -amt, accounts[from_idx].balance, note1);
    log_transaction(accounts[to_idx].acc_no, "UPI_IN", amt, accounts[to_idx].balance, note2);
    push_notification(accounts[to_idx].acc_no, "You received money via UPI.");
    printf("UPI transfer completed. New balance: %.2f INR\n", accounts[from_idx].balance);
}

/* UPI transfer (auth inside) convenience */
static void upi_transfer_interactive(void) {
    int idx = authenticate_prompt(); if (idx < 0) return;
    upi_transfer_from_loggedin(idx);
}

/* mini-statement */
static void print_mini_statement_for_account(int acc_no) {
    FILE *f = fopen(F_TRANSACTIONS, "r");
    if (!f) { printf("No transactions yet.\n"); return; }
    char *lines[MINI_STAT_LIMIT];
    for (int i = 0; i < MINI_STAT_LIMIT; ++i) lines[i] = NULL;
    char buf[MAX_LINE];
    while (fgets(buf, sizeof buf, f)) {
        int txn_acc = 0;
        if (sscanf(buf, "%d|", &txn_acc) != 1) continue;
        if (txn_acc != acc_no) continue;
        if (lines[0]) { free(lines[0]); lines[0] = NULL; }
        for (int j = 1; j < MINI_STAT_LIMIT; ++j) lines[j-1] = lines[j];
        lines[MINI_STAT_LIMIT-1] = strdup(buf);
    }
    fclose(f);
    printf("Mini-statement (last %d):\n", MINI_STAT_LIMIT);
    for (int i = 0; i < MINI_STAT_LIMIT; ++i) {
        if (lines[i]) { printf("%s", lines[i]); free(lines[i]); }
    }
}

/* ---------------- Trading: list, buy, sell ---------------- */

static void ensure_default_prices(void) {
    if (price_count > 0) return;
    /* create a few default assets across markets */
    price_count = 0;
    PriceRec p;
    get_timestamp(p.last_update, sizeof p.last_update);

    strncpy(p.asset_id, "INFY", sizeof p.asset_id - 1);
    strncpy(p.asset_name, "Infosys Ltd", sizeof p.asset_name - 1);
    p.price = 1500.0; p.vol = 0.01; strncpy(p.market, "IN", sizeof p.market - 1);
    p.open_hour = 9; p.close_hour = 15; prices[price_count++] = p;

    strncpy(p.asset_id, "TCS", sizeof p.asset_id - 1);
    strncpy(p.asset_name, "TCS", sizeof p.asset_name - 1);
    p.price = 3200.0; p.vol = 0.008; strncpy(p.market, "IN", sizeof p.market - 1);
    p.open_hour = 9; p.close_hour = 15; prices[price_count++] = p;

    strncpy(p.asset_id, "AAPL", sizeof p.asset_id - 1);
    strncpy(p.asset_name, "Apple Inc", sizeof p.asset_name - 1);
    p.price = 190.0; p.vol = 0.02; strncpy(p.market, "US", sizeof p.market - 1);
    p.open_hour = 9; p.close_hour = 17; prices[price_count++] = p;

    strncpy(p.asset_id, "NVDA", sizeof p.asset_id - 1);
    strncpy(p.asset_name, "NVIDIA Corp", sizeof p.asset_name - 1);
    p.price = 190.0; p.vol = 0.03; strncpy(p.market, "US", sizeof p.market - 1);
    p.open_hour = 9; p.close_hour = 17; prices[price_count++] = p;

    strncpy(p.asset_id, "BTC", sizeof p.asset_id - 1);
    strncpy(p.asset_name, "Bitcoin", sizeof p.asset_name - 1);
    p.price = 35000.0; p.vol = 0.05; strncpy(p.market, "US", sizeof p.market - 1);
    p.open_hour = 0; p.close_hour = 24; prices[price_count++] = p;

    strncpy(p.asset_id, "SIE", sizeof p.asset_id - 1);
    strncpy(p.asset_name, "Siemens", sizeof p.asset_name - 1);
    p.price = 120.0; p.vol = 0.018; strncpy(p.market, "EU", sizeof p.market - 1);
    p.open_hour = 8; p.close_hour = 18; prices[price_count++] = p;

    save_prices();
    audit_log("INITIALIZED_DEFAULT_PRICES");
}

/* list prices — if market open do a tick (simulate live) before listing */
static void list_market_prices(void) {
    ensure_default_prices();
    /* do a tick for each market that is open */
    tick_market_once();
    printf("AssetID  Market  AssetName                Price (native)\n");
    for (int i = 0; i < price_count; ++i) {
        PriceRec *p = &prices[i];
        printf("%-7s  %-5s  %-22s  %.4f    (last: %s)\n",
            p->asset_id, p->market, p->asset_name, p->price, p->last_update);
    }
}

/* helper: convert price (native) and quantity to INR cost */
static double cost_in_inr_for_purchase(const PriceRec *p, double qty) {
    double cost_native = p->price * qty;
    if (strcmp(p->market, "IN") == 0) return cost_native;
    if (strcmp(p->market, "US") == 0) return cost_native * fx.inr_per_usd;
    if (strcmp(p->market, "EU") == 0) return cost_native * fx.inr_per_eur;
    return cost_native;
}

/* buy asset while logged in */
static void buy_asset_loggedin(int acc_idx) {
    if (acc_idx < 0) return;
    ensure_default_prices();
    char buf[128];
    printf("Enter Asset ID to buy (e.g., AAPL): ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf); /* ensure case-sensitive IDs as stored */
    int pidx = find_price_index(buf);
    if (pidx < 0) { printf("Asset not found.\n"); return; }
    PriceRec *pr = &prices[pidx];
    if (!market_is_open(pr)) {
        printf("Market for %s (%s) is currently closed (open %02d:00 to %02d:00).\n", pr->asset_id, pr->market, pr->open_hour, pr->close_hour);
        return;
    }
    printf("Current price of %s (%s) = %.4f (native)\n", pr->asset_name, pr->asset_id, pr->price);
    printf("Enter quantity to buy: ");
    double qty = safe_read_double();
    if (qty <= 0) { printf("Invalid quantity.\n"); return; }
    double cost_inr = cost_in_inr_for_purchase(pr, qty);
    if (cost_inr > accounts[acc_idx].balance) { printf("Insufficient cash (need %.2f INR).\n", cost_inr); return; }

    /* deduct cash */
    accounts[acc_idx].balance -= cost_inr;

    /* update or add holding */
    int hidx = find_holding_index(accounts[acc_idx].acc_no, pr->asset_id);
    if (hidx < 0) {
        if (hold_count >= MAX_HOLDINGS) { printf("Holdings limit reached.\n"); return; }
        Holding h;
        h.acc_no = accounts[acc_idx].acc_no;
        strncpy(h.asset_id, pr->asset_id, sizeof h.asset_id - 1);
        strncpy(h.asset_name, pr->asset_name, sizeof h.asset_name - 1);
        h.qty = qty;
        h.avg_price = pr->price;
        strncpy(h.market, pr->market, sizeof h.market - 1);
        holdings[hold_count++] = h;
    } else {
        Holding *h = &holdings[hidx];
        /* avg price in native currency */
        double total_old = h->avg_price * h->qty;
        double total_new = pr->price * qty;
        h->qty += qty;
        if (h->qty > 0.0) h->avg_price = (total_old + total_new) / h->qty;
    }
    save_accounts(); save_holdings();
    char note[128]; snprintf(note, sizeof note, "Bought %s x %.4f", pr->asset_id, qty);
    log_transaction(accounts[acc_idx].acc_no, "BUY", -cost_inr, accounts[acc_idx].balance, note);
    char audit[128]; snprintf(audit, sizeof audit, "BUY|%d|%s|%.4f|%.2fINR", accounts[acc_idx].acc_no, pr->asset_id, qty, cost_inr);
    audit_log(audit);
    push_notification(accounts[acc_idx].acc_no, note);
    printf("Bought %s x %.4f for %.2f INR. New cash balance: %.2f INR\n", pr->asset_id, qty, cost_inr, accounts[acc_idx].balance);
}

/* sell asset while logged in */
static void sell_asset_loggedin(int acc_idx) {
    if (acc_idx < 0) return;
    ensure_default_prices();
    char buf[128];
    printf("Enter Asset ID to sell: ");
    if (!fgets(buf, sizeof buf, stdin)) return;
    trim_newline(buf);
    int pidx = find_price_index(buf);
    if (pidx < 0) { printf("Asset unknown.\n"); return; }
    int hidx = find_holding_index(accounts[acc_idx].acc_no, buf);
    if (hidx < 0) { printf("You do not own this asset.\n"); return; }
    Holding *h = &holdings[hidx];
    PriceRec *pr = &prices[pidx];
    printf("You own %.6f units. Current price (native) = %.4f\n", h->qty, pr->price);
    printf("Quantity to sell: ");
    double qty = safe_read_double();
    if (qty <= 0 || qty > h->qty) { printf("Invalid quantity.\n"); return; }
    double proceeds_inr = cost_in_inr_for_purchase(pr, qty); /* reuse function */
    /* reduce holdings */
    h->qty -= qty;
    if (h->qty <= 0.000001) {
        for (int i = hidx; i < hold_count - 1; ++i) holdings[i] = holdings[i+1];
        hold_count--;
    }
    accounts[acc_idx].balance += proceeds_inr;
    save_accounts(); save_holdings();
    char note[128]; snprintf(note, sizeof note, "Sold %s x %.4f", pr->asset_id, qty);
    log_transaction(accounts[acc_idx].acc_no, "SELL", proceeds_inr, accounts[acc_idx].balance, note);
    char audit[128]; snprintf(audit, sizeof audit, "SELL|%d|%s|%.4f|%.2fINR", accounts[acc_idx].acc_no, pr->asset_id, qty, proceeds_inr);
    audit_log(audit);
    push_notification(accounts[acc_idx].acc_no, note);
    printf("Sold %.4f units, credited %.2f INR. New cash: %.2f INR\n", qty, proceeds_inr, accounts[acc_idx].balance);
}

/* view portfolio with P/L (colored) */
static void view_portfolio(int acc_idx) {
    if (acc_idx < 0) return;
    printf("Holdings for account %d (%s):\n", accounts[acc_idx].acc_no, accounts[acc_idx].name);
    printf("AssetID  Market  Qty       AvgPrice(native)  CurPrice(native)  Value(INR)   P/L(INR)\n");
    for (int i = 0; i < hold_count; ++i) {
        if (holdings[i].acc_no != accounts[acc_idx].acc_no) continue;
        Holding *h = &holdings[i];
        int pidx = find_price_index(h->asset_id);
        double cur_native = (pidx >= 0) ? prices[pidx].price : h->avg_price;
        double cur_inr;
        if (strcmp(h->market, "IN") == 0) cur_inr = cur_native;
        else if (strcmp(h->market, "US") == 0) cur_inr = cur_native * fx.inr_per_usd;
        else if (strcmp(h->market, "EU") == 0) cur_inr = cur_native * fx.inr_per_eur;
        else cur_inr = cur_native;
        double value_inr = h->qty * cur_inr;
        double avg_inr = h->avg_price * (strcmp(h->market,"US")==0 ? fx.inr_per_usd : (strcmp(h->market,"EU")==0 ? fx.inr_per_eur : 1.0));
        double pl = h->qty * (cur_inr - avg_inr);
        const char *color = pl >= 0 ? ANSI_GREEN : ANSI_RED;
        printf("%-7s  %-6s  %-8.4f  %-16.4f  %-16.4f  %-11.2f  %s%+.2f%s\n",
            h->asset_id, h->market, h->qty, h->avg_price, cur_native, value_inr, color, pl, ANSI_RESET);
    }
    double port = compute_portfolio_value_inr(accounts[acc_idx].acc_no);
    double pl_total = compute_unrealized_pl_inr(accounts[acc_idx].acc_no);
    const char *color = pl_total >= 0 ? ANSI_GREEN : ANSI_RED;
    printf("Portfolio Value: %.2f INR  |  Unrealized P/L: %s%+.2f INR%s\n", port, color, pl_total, ANSI_RESET);
}

/* ---------------- New UI: Account Details ---------------- */
static void show_account_details(int idx) {
    if (idx < 0 || idx >= acc_count) { printf("Invalid account.\n"); return; }
    Account *a = &accounts[idx];
    printf("\n--- Account Details ---\n");
    printf("Account Number : %d\n", a->acc_no);
    printf("Name           : %s\n", a->name);
    printf("Account Type   : %s\n", a->acc_type);
    printf("UPI            : %s\n", a->upi);
    printf("Cash Balance   : %.2f INR\n", a->balance);
    printf("Loan Outstanding: %.2f INR\n", a->loan);
    printf("Status         : %s\n", a->active ? "Active" : "Inactive");
    printf("Frozen         : %s\n", a->frozen ? "Yes" : "No");
    printf("Last Login     : %s\n", a->last_login);
    printf("------------------------\n");
}

/* ---------------- Admin functions ---------------- */

static void admin_menu(void) {
    printf("Admin PIN: ");
    int pin = safe_read_int();
    if (pin != ADMIN_PIN) { printf("Invalid admin PIN.\n"); return; }
    audit_log("ADMIN_LOGIN");
    for (;;) {
        printf("\n--- Admin Dashboard ---\n");
        printf("1.View accounts\n2.Set price\n3.Randomize prices (admin)\n4.Apply interest to Savings\n5.View audit log file path\n6.Set FX rates\n7.Unfreeze account\n8.Tick market once\n0.Logout\nChoice: ");
        int ch = safe_read_int();
        if (ch == 1) {
            printf("AccNo | Name | Type | Balance | Loan | Active | Frozen | UPI\n");
            for (int i = 0; i < acc_count; ++i) {
                Account *a = &accounts[i];
                printf("%d | %s | %s | %.2f | %.2f | %d | %d | %s\n",
                    a->acc_no, a->name, a->acc_type, a->balance, a->loan, a->active, a->frozen, a->upi);
            }
        } else if (ch == 2) {
            printf("Enter Asset ID to set price: ");
            char buf[128]; if (!fgets(buf, sizeof buf, stdin)) break; trim_newline(buf);
            int idx = find_price_index(buf);
            if (idx < 0) { printf("Asset not found.\n"); continue; }
            printf("Enter new price (native): ");
            double p = safe_read_double(); if (p <= 0) { printf("Invalid.\n"); continue; }
            double old = prices[idx].price; prices[idx].price = p; get_timestamp(prices[idx].last_update, sizeof prices[idx].last_update); save_prices();
            char audit[128]; snprintf(audit, sizeof audit, "ADMIN_SET_PRICE|%s|%.4f->%.4f", prices[idx].asset_id, old, p); audit_log(audit);
            printf("Price updated.\n");
        } else if (ch == 3) {
            admin_randomize_all_prices();
            printf("Prices randomized by admin.\n");
        } else if (ch == 4) {
            printf("Enter annual interest percent for Savings: ");
            double rate = safe_read_double();
            if (rate <= 0) { printf("Invalid rate.\n"); continue; }
            for (int i = 0; i < acc_count; ++i) {
                if (!accounts[i].active) continue;
                if (bvdu_stricmp(accounts[i].acc_type, "Savings") == 0) {
                    double interest = accounts[i].balance * (rate / 100.0);
                    accounts[i].balance += interest;
                    char note[128]; snprintf(note, sizeof note, "Interest applied %.2f%%", rate);
                    log_transaction(accounts[i].acc_no, "INTEREST", interest, accounts[i].balance, note);
                }
            }
            save_accounts();
            audit_log("ADMIN_APPLY_INTEREST");
            printf("Interest applied to savings.\n");
        } else if (ch == 5) {
            printf("Audit log file: %s\nNotifications file: %s\n", F_ADMIN_AUDIT, F_NOTIFICATIONS);
        } else if (ch == 6) {
            printf("Enter INR per USD (e.g., 83.5): ");
            double usd = safe_read_double();
            printf("Enter INR per EUR (e.g., 88.2): ");
            double eur = safe_read_double();
            if (usd <= 0 || eur <= 0) { printf("Invalid rates.\n"); continue; }
            fx.inr_per_usd = usd; fx.inr_per_eur = eur; get_timestamp(fx.last_update, sizeof fx.last_update); save_fx();
            char audit[128]; snprintf(audit, sizeof audit, "ADMIN_SET_FX|INR_USD=%.6f|INR_EUR=%.6f", usd, eur); audit_log(audit);
            printf("FX updated.\n");
        } else if (ch == 7) {
            printf("Enter acc_no to unfreeze: ");
            int a = safe_read_int();
            int idx = find_account_index(a);
            if (idx < 0) { printf("Account not found.\n"); continue; }
            accounts[idx].frozen = 0; accounts[idx].failed_attempts = 0; save_accounts();
            char audit[128]; snprintf(audit, sizeof audit, "ADMIN_UNFREEZE|%d", a); audit_log(audit);
            printf("Account %d unfrozen.\n", a);
        } else if (ch == 8) {
            tick_market_once();
            printf("Market tick executed.\n");
        } else if (ch == 0) {
            audit_log("ADMIN_LOGOUT"); break;
        } else printf("Invalid.\n");
    }
}

/* ---------------- Menus ---------------- */

static void trading_app_menu(int acc_idx) {
    if (acc_idx < 0) {
        printf("Please login as a customer to use the Trading App.\n");
        return;
    }
    ensure_default_prices();
    for (;;) {
        printf("\n=== BVDU Trading App ===\n1.List Market Prices\n2.Buy Asset\n3.Sell Asset\n4.View Portfolio\n0.Exit\nChoice: ");
        int ch = safe_read_int();
        if (ch == 1) list_market_prices();
        else if (ch == 2) buy_asset_loggedin(acc_idx);
        else if (ch == 3) sell_asset_loggedin(acc_idx);
        else if (ch == 4) view_portfolio(acc_idx);
        else if (ch == 0) break;
        else printf("Invalid.\n");
    }
}

/* Customer dashboard uses logged-in index and avoids re-auth for transfer/upi */
static void customer_dashboard(int idx) {
    if (idx < 0) return;
    for (;;) {
        double port = compute_portfolio_value_inr(accounts[idx].acc_no);
        double pl = compute_unrealized_pl_inr(accounts[idx].acc_no);
        printf("\n--- Customer Dashboard: %s (%d) ---\n", accounts[idx].name, accounts[idx].acc_no);
        printf("Cash: %.2f INR | Portfolio: %.2f INR | Unrealized P/L: %+.2f INR\n", accounts[idx].balance, port, pl);
        printf("1.Balance Enquiry\n2.Deposit\n3.Withdraw\n4.Transfer\n5.Mini Statement\n6.Trading App\n7.UPI Transfer\n8.Account Details\n0.Logout\nChoice: ");
        int ch = safe_read_int();
        if (ch == 1) { printf("Cash balance: %.2f INR\nLoan outstanding: %.2f\n", accounts[idx].balance, accounts[idx].loan); }
        else if (ch == 2) deposit_money();
        else if (ch == 3) withdraw_money();
        else if (ch == 4) transfer_from_loggedin(idx);
        else if (ch == 5) print_mini_statement_for_account(accounts[idx].acc_no);
        else if (ch == 6) trading_app_menu(idx);
        else if (ch == 7) upi_transfer_from_loggedin(idx);
        else if (ch == 8) show_account_details(idx);
        else if (ch == 0) { printf("Logging out...\n"); break; }
        else printf("Invalid.\n");
    }
}

/* ---------------- Initialization: create default files if missing ---------------- */

static void ensure_default_files(void) {
    /* accounts: if not present, create sample team accounts */
    FILE *f;
    f = fopen(F_ACCOUNTS, "r"); if (!f) {
        fclose(f);
        /* create sample accounts */
        Account a;
        acc_count = 0;
        /* sample team members */
        a.acc_no = 1001; strncpy(a.name, "adarsh", sizeof a.name-1); a.name[sizeof a.name-1]='\0'; strncpy(a.acc_type, "Savings", sizeof a.acc_type-1); a.pin = 1234; a.balance = 10000.0; a.loan = 0; a.active=1; a.frozen=0; a.failed_attempts=0; strncpy(a.upi,"adarsh@bvdu", sizeof a.upi-1); get_timestamp(a.last_login, sizeof a.last_login); accounts[acc_count++]=a;
        a.acc_no = 1002; strncpy(a.name, "achyut", sizeof a.name-1); a.name[sizeof a.name-1]='\0'; strncpy(a.acc_type, "Savings", sizeof a.acc_type-1); a.pin = 2345; a.balance = 8000.0; a.loan = 0; a.active=1; a.frozen=0; a.failed_attempts=0; strncpy(a.upi,"achyut@bvdu", sizeof a.upi-1); get_timestamp(a.last_login, sizeof a.last_login); accounts[acc_count++]=a;
        a.acc_no = 1003; strncpy(a.name, "ayush", sizeof a.name-1); a.name[sizeof a.name-1]='\0'; strncpy(a.acc_type, "Current", sizeof a.acc_type-1); a.pin = 3456; a.balance = 5000.0; a.loan = 0; a.active=1; a.frozen=0; a.failed_attempts=0; strncpy(a.upi,"ayush@bvdu", sizeof a.upi-1); get_timestamp(a.last_login, sizeof a.last_login); accounts[acc_count++]=a;
        a.acc_no = 1004; strncpy(a.name, "aabir", sizeof a.name-1); a.name[sizeof a.name-1]='\0'; strncpy(a.acc_type, "Savings", sizeof a.acc_type-1); a.pin = 4567; a.balance = 12000.0; a.loan = 0; a.active=1; a.frozen=0; a.failed_attempts=0; strncpy(a.upi,"aabir@bvdu", sizeof a.upi-1); get_timestamp(a.last_login, sizeof a.last_login); accounts[acc_count++]=a;
        save_accounts();
        audit_log("DEFAULT_ACCOUNTS_CREATED");
    } else fclose(f);

    /* transactions */
    f = fopen(F_TRANSACTIONS, "r"); if (!f) { fclose(f); FILE *t = fopen(F_TRANSACTIONS, "w"); if (t) fclose(t); }
    else fclose(f);

    /* holdings - if absent create empty */
    f = fopen(F_HOLDINGS, "r"); if (!f) { fclose(f); FILE *t = fopen(F_HOLDINGS, "w"); if (t) fclose(t); }
    else fclose(f);

    /* prices */
    f = fopen(F_PRICES, "r");
    if (!f) {
        fclose(f);
        /* ensure_default_prices will create default prices and save */
        ensure_default_prices();
    } else fclose(f);

    /* fx */
    f = fopen(F_FX, "r");
    if (!f) {
        fclose(f);
        get_timestamp(fx.last_update, sizeof fx.last_update);
        save_fx();
    } else fclose(f);

    /* admin audit and notifications */
    f = fopen(F_ADMIN_AUDIT, "a"); if (f) fclose(f);
    f = fopen(F_NOTIFICATIONS, "a"); if (f) fclose(f);
}

/* ---------------- Main menu and entry ---------------- */

int main(void) {
    srand((unsigned)time(NULL));
    load_fx();
    load_prices();
    load_holdings();
    load_accounts();
    ensure_default_files();

    printf("=== BVDU Bank — Banking & Trading Management System ===\n");
    for (;;) {
        printf("\nMain Menu:\n1.Customer Login\n2.Create Account\n3.List Market Prices\n4.Admin\n0.Exit\nChoice: ");
        int ch = safe_read_int();
        if (ch == 1) {
            int idx = authenticate_prompt();
            if (idx >= 0) customer_dashboard(idx);
        } else if (ch == 2) create_account_interactive();
        else if (ch == 3) list_market_prices();
        else if (ch == 4) admin_menu();
        else if (ch == 0) { printf("Bye — saving data...\n"); save_accounts(); save_holdings(); save_prices(); save_fx(); break; }
        else printf("Invalid.\n");
    }
    return 0;
}

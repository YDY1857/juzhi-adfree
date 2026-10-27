/*
 * AdFree.c — 橘汁影视 去广告注入库  【侦察版 / 只处理开屏】
 *
 * 这一版的目的：让库自己上报它活到了哪一步。
 * 日志写到沙盒 Documents/adfree.log，安装后必读。
 *
 * 编译：标准链接（libobjc + Foundation），不再依赖已废弃的 flat namespace。
 */

typedef void *id_t;

extern void *objc_msgSend(void *self, void *op, ...);
extern void *sel_registerName(const char *name);
extern void *objc_getClass(const char *name);
extern int objc_getClassList(void **buffer, int count);
extern const char *class_getName(void *cls);
extern void *object_getClass(void *obj);
extern void **class_copyMethodList(void *cls, unsigned int *count);
extern void *method_getName(void *m);
extern const char *sel_getName(void *sel);
extern void *method_getImplementation(void *m);
extern void method_setImplementation(void *m, void *imp);
extern void free(void *p);
extern int strncmp(const char *a, const char *b, unsigned long n);
extern char *strstr(const char *h, const char *n);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *d, const char *s);
extern char *strcat(char *d, const char *s);
extern int sprintf(char *buf, const char *fmt, ...);
extern char *getenv(const char *name);
extern int open(const char *path, int flags, ...);
extern long write(int fd, const void *buf, unsigned long n);
extern int close(int fd);
extern void dispatch_async_f(void *queue, void *ctx, void (*fn)(void *));
extern void *dispatch_get_main_queue(void);
extern unsigned long long dispatch_time(unsigned long long when, long long delta);
extern void dispatch_after_f(unsigned long long when, void *q, void *ctx, void (*fn)(void *));

#define S(x) sel_registerName(x)
#define O_WRONLY 0x0001
#define O_CREAT  0x0200
#define O_APPEND 0x0008

/* ---------------- 日志 ---------------- */
static char g_logpath[512];

static void log_init(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    strcpy(g_logpath, home);
    strcat(g_logpath, "/Documents/adfree.log");
}

static void mark(const char *s) {
    if (!g_logpath[0]) return;
    int fd = open(g_logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, s, strlen(s));
    write(fd, "\n", 1);
    close(fd);
}

static void markf(const char *fmt, int a) {
    char b[256];
    sprintf(b, fmt, a);
    mark(b);
}

static void marks(const char *tag, const char *v) {
    char b[256];
    sprintf(b, "%s%s", tag, v ? v : "(null)");
    mark(b);
}

/* ---------------- 运行时辅助 ---------------- */
static void *msg0(void *o, const char *sel) {
    return ((void *(*)(void *, void *))objc_msgSend)(o, S(sel));
}

static int responds(void *o, const char *sel) {
    if (!o) return 0;
    return (int)(((unsigned char (*)(void *, void *, void *))objc_msgSend)(
        o, S("respondsToSelector:"), S(sel)));
}

static void call2(void *o, const char *sel, void *a, void *b) {
    ((void (*)(void *, void *, void *, void *))objc_msgSend)(o, S(sel), a, b);
}

static void *make_error(void) {
    void *nsstr = objc_getClass("NSString");
    void *nserr = objc_getClass("NSError");
    if (!nsstr || !nserr) return 0;
    void *domain = ((void *(*)(void *, void *, const char *))objc_msgSend)(
        nsstr, S("stringWithUTF8String:"), "com.zc.adfree");
    return ((void *(*)(void *, void *, void *, long, void *))objc_msgSend)(
        nserr, S("errorWithDomain:code:userInfo:"), domain, -1L, 0);
}

/* ---------------- 只处理开屏 ---------------- */
static void notify_fail(void *obj) {
    void *dg = msg0(obj, "delegate");
    const char *cn = class_getName(object_getClass(obj));
    marks("F class=", cn);
    if (!dg) { mark("F  no-delegate"); return; }

    void *err = make_error();
    const char *cands[3] = {
        "splashAdLoadFail:error:",
        "splashAd:didFailWithError:",
        0
    };
    for (int i = 0; cands[i]; i++) {
        if (responds(dg, cands[i])) {
            marks("F  call=", cands[i]);
            call2(dg, cands[i], obj, err);
            return;
        }
    }
    mark("F  no-delegate-method");
}

static void fail_worker(void *ctx) {
    void *obj = ctx;
    notify_fail(obj);
    msg0(obj, "release");
    mark("F  worker-done");
}

static long imp_load(void *self, void *cmd) {
    (void)cmd;
    mark("L load-called");
    msg0(self, "retain");
    dispatch_async_f(dispatch_get_main_queue(), self, fail_worker);
    return 0;
}

static long imp_show(void *self, void *cmd) {
    (void)self; (void)cmd;
    mark("W show-blocked");
    return 0;
}

static const char *kLoadSels[] = {
    "loadAdData", "loadAdDataWithCount:", "loadAdDataWithCompletionHandler:",
    "loadSplashAdWithSlotID:andParameter:", "loadFullScreenAd", "loadHalfScreenAd", 0
};
static const char *kShowSels[] = {
    "showInWindow:", "showInWindowWithBlock:", "showAdFromRootViewController:",
    "showAdInWindow:withBottomView:skipView:", 0
};

static int is_splash_class(const char *n) {
    static const char *pfx[] = {"BU", "CSJ", "ABU", "GM", "GDT", "KS", 0};
    int ok = 0;
    for (int i = 0; pfx[i]; i++)
        if (strncmp(n, pfx[i], strlen(pfx[i])) == 0) { ok = 1; break; }
    if (!ok) return 0;
    return strstr(n, "SplashAd") != 0;
}

static int hook_list(void *cls) {
    int n = 0;
    if (!cls) return 0;
    unsigned int cnt = 0;
    void **ms = class_copyMethodList(cls, &cnt);
    if (!ms) return 0;
    for (unsigned int i = 0; i < cnt; i++) {
        void *m = ms[i];
        const char *sn = sel_getName(method_getName(m));
        if (!sn) continue;
        void *t = 0;
        for (int k = 0; kLoadSels[k]; k++)
            if (strstr(sn, kLoadSels[k])) { t = (void *)imp_load; break; }
        if (!t)
            for (int k = 0; kShowSels[k]; k++)
                if (strstr(sn, kShowSels[k])) { t = (void *)imp_show; break; }
        if (!t) continue;
        if (method_getImplementation(m) == t) continue;
        method_setImplementation(m, t);
        n++;
    }
    free(ms);
    return n;
}

/* ---------------- 入口 ---------------- */
__attribute__((constructor)) static void adfree_init(void) {
    log_init();
    mark("=== S0 constructor ===");
    marks("HOME=", getenv("HOME"));

    const char *probe[] = {
        "CSJSplashAd", "BUSplashAd", "ABUSplashAd", "GMSplashAd",
        "BUNativeAdsManager", "BUNativeExpressFullscreenVideoAd",
        "BUNativeExpressRewardedVideoAd", "ZCBundleGarrisonSaddle", 0
    };
    for (int i = 0; probe[i]; i++) {
        void *c = objc_getClass(probe[i]);
        marks(c ? "probe OK   " : "probe MISS ", probe[i]);
    }

    int total = objc_getClassList(0, 0);
    markf("S1 classes=%d", total);

    static void *buf[16384];
    int n = objc_getClassList(buf, total > 16384 ? 16384 : total);
    int matched = 0, hooked = 0;
    for (int i = 0; i < n; i++) {
        void *cls = buf[i];
        const char *cn = class_getName(cls);
        if (!cn || !is_splash_class(cn)) continue;
        matched++;
        marks("S2 splash-class: ", cn);
        hooked += hook_list(cls);
        hooked += hook_list(object_getClass(cls));
    }
    markf("S2 matched=%d", matched);
    markf("S2 hooked=%d", hooked);
    mark("=== S3 init done ===");
}

static void alive_check(void *ctx) {
    (void)ctx;
    mark("S4 alive (still running)");
}

__attribute__((constructor(201))) static void adfree_late(void) {
    /* 1 秒后再打个点，证明进程没死在初始化阶段 */
    dispatch_after_f(dispatch_time(0, 1000000000LL), dispatch_get_main_queue(), 0, alive_check);
}

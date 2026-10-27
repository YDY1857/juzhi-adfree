/*
 * AdFree.c — 橘汁影视 去广告注入库 (arm64 / iOS)
 *
 * 目标 App 是 Flutter 壳，广告由自研原生插件 ZCBundleGarrisonSaddle 直连
 * 穿山甲 (CSJ / GroMore-ABU) 展示，共 4 类：
 *   开屏   -> CSJSplashAd / BUSplashAd        delegate: splashAdLoadFail:error:
 *   信息流 -> *NativeAdsManager               delegate: nativeAdsManager:didFailWithError:
 *   插屏   -> *ExpressFullscreenVideoAd       delegate: nativeExpressFullscreenVideoAd:didFailWithError:
 *   激励   -> *ExpressRewardedVideoAd         delegate: nativeExpressRewardedVideoAd:didFailWithError:
 *
 * 做法：把 SDK 的 load* 入口短路，并异步投递一次「无填充」失败回调，
 * 等价于线上真实的广告无填充场景，App 自己的容错逻辑会照常往下走，
 * 因此不会白屏、不会卡开屏；show* 入口一律吞掉，任何广告 UI 都不出现。
 *
 * 不依赖 CydiaSubstrate，纯 ObjC runtime；不做 include，
 * 全部符号运行时解析，因此也能在 Windows 上用 zig 交叉编译验证。
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
extern void dispatch_async_f(void *queue, void *ctx, void (*fn)(void *));
extern void *dispatch_get_main_queue(void);

#define S(x) sel_registerName(x)

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

/* 构造 NSError，供失败回调使用 */
static void *make_error(void) {
    void *nsstr = objc_getClass("NSString");
    void *nserr = objc_getClass("NSError");
    if (!nsstr || !nserr) return 0;
    void *domain = ((void *(*)(void *, void *, const char *))objc_msgSend)(
        nsstr, S("stringWithUTF8String:"), "com.zc.adfree");
    return ((void *(*)(void *, void *, void *, long, void *))objc_msgSend)(
        nserr, S("errorWithDomain:code:userInfo:"), domain, -1L, 0);
}

/* 按类名挑对应的失败回调（顺序即优先级） */
static void notify_fail(void *obj) {
    void *dg = msg0(obj, "delegate");
    if (!dg) return;

    const char *cn = class_getName(object_getClass(obj));
    if (!cn) return;

    const char *cand[4] = {0, 0, 0, 0};
    if (strstr(cn, "Splash")) {
        cand[0] = "splashAdLoadFail:error:";
        cand[1] = "splashAd:didFailWithError:";
    } else if (strstr(cn, "NativeAdsManager") || strstr(cn, "NativeAdManager")) {
        cand[0] = "nativeAdsManager:didFailWithError:";
        cand[1] = "nativeAdManager:didFailWithError:";
    } else if (strstr(cn, "ExpressFullscreenVideoAd") ||
               (strstr(cn, "FullscreenVideoAd") && !strstr(cn, "Rewarded"))) {
        cand[0] = "nativeExpressFullscreenVideoAd:didFailWithError:";
        cand[1] = "fullscreenVideoAd:didFailWithError:";
    } else if (strstr(cn, "ExpressRewardedVideoAd") || strstr(cn, "RewardedVideoAd")) {
        cand[0] = "nativeExpressRewardedVideoAd:didFailWithError:";
        cand[1] = "rewardedVideoAd:didFailWithError:";
    } else if (strstr(cn, "Interstitial")) {
        cand[0] = "interstitialAd:didFailWithError:";
        cand[1] = "nativeExpressInterstitialAd:didFailWithError:";
    } else if (strstr(cn, "Banner")) {
        cand[0] = "nativeExpressBannerAdView:didLoadFailWithError:";
        cand[1] = "bannerAdView:didFailWithError:";
    } else {
        return;
    }

    void *err = make_error();
    for (int i = 0; i < 4; i++) {
        if (!cand[i]) continue;
        if (responds(dg, cand[i])) {
            call2(dg, cand[i], obj, err);
            return;
        }
    }
}

/* 异步工作：对象保活 -> 投递失败回调 -> 释放 */
static void fail_worker(void *ctx) {
    void *obj = ctx;
    notify_fail(obj);
    msg0(obj, "release");
}

/* 替换实现：load* 一律短路并回报失败；show* 一律吞掉 */
static long imp_load(void *self, void *cmd) {
    (void)cmd;
    msg0(self, "retain");
    dispatch_async_f(dispatch_get_main_queue(), self, fail_worker);
    return 0;
}
static long imp_show(void *self, void *cmd) {
    (void)self; (void)cmd;
    return 0;
}

static const char *kLoadSels[] = {
    "loadAdData",
    "loadAdDataWithCount:",
    "loadAdDataWithCompletionHandler:",
    "loadSplashAdWithSlotID:andParameter:",
    "loadFullScreenAd",
    "loadHalfScreenAd",
    "loadAdAndShow",
    0
};

static const char *kShowSels[] = {
    "showAdFromRootViewController:",
    "showAdFromRootViewController:extraInfos:",
    "showAdFromRootViewController:extroInfos:",
    "showAdFromRootViewController:ritScene:ritSceneDescribe:",
    "showAdFromRootViewController:ritSceneDescribe:",
    "showInWindow:",
    "showInWindowWithBlock:",
    "showAdInWindow:withBottomView:skipView:",
    "showFromRootViewController:",
    0
};

/* 只处理广告 SDK 的类，避开 App 自身混淆过的 ZC* 类 */
static int is_ad_class(const char *n) {
    static const char *pfx[] = {"BU", "CSJ", "ABU", "GM", "GDT", "KS", "TT", "BD", 0};
    int ok = 0;
    for (int i = 0; pfx[i]; i++) {
        if (strncmp(n, pfx[i], strlen(pfx[i])) == 0) { ok = 1; break; }
    }
    if (!ok) return 0;

    static const char *mk[] = {
        "SplashAd", "NativeAdsManager", "NativeAdManager",
        "ExpressFullscreenVideoAd", "ExpressRewardedVideoAd",
        "FullscreenVideoAd", "RewardedVideoAd", "InterstitialAd",
        "NativeExpressBannerView", "BannerAd", "ABUBaseAd",
        "NativeExpressAdManager", "DrawAdsManager",
        0
    };
    for (int i = 0; mk[i]; i++) if (strstr(n, mk[i])) return 1;
    return 0;
}

static void hook_method_list(void *cls) {
    if (!cls) return;
    unsigned int cnt = 0;
    void **ms = class_copyMethodList(cls, &cnt);
    if (!ms) return;
    for (unsigned int i = 0; i < cnt; i++) {
        void *m = ms[i];
        const char *sn = sel_getName(method_getName(m));
        if (!sn) continue;

        void *target = 0;
        for (int k = 0; kLoadSels[k]; k++) {
            if (strstr(sn, kLoadSels[k])) { target = (void *)imp_load; break; }
        }
        if (!target) {
            for (int k = 0; kShowSels[k]; k++) {
                if (strstr(sn, kShowSels[k])) { target = (void *)imp_show; break; }
            }
        }
        if (!target) continue;
        if (method_getImplementation(m) == target) continue;
        method_setImplementation(m, target);
    }
    free(ms);
}

static void hook_all(void) {
    static void *buf[16384];
    int n = objc_getClassList(0, 0);
    if (n <= 0) return;
    if (n > 16384) n = 16384;
    n = objc_getClassList(buf, n);
    for (int i = 0; i < n; i++) {
        void *cls = buf[i];
        const char *cn = class_getName(cls);
        if (!cn || !is_ad_class(cn)) continue;
        hook_method_list(cls);            /* 实例方法 */
        hook_method_list(object_getClass(cls)); /* 类方法（metaclass） */
    }
}

__attribute__((constructor)) static void adfree_init(void) {
    hook_all();
}

/* 兜底：再扫一遍，捕获延迟注册/延迟 dlopen 的类 */
__attribute__((constructor(201))) static void adfree_init_late(void) {
    hook_all();
}

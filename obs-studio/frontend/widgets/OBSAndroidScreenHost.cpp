#include "OBSAndroidPermissions.hpp"
#include "OBSAndroidScreenGpuHost.hpp"
#include "ScreenAudioStartTransaction.hpp"

#ifdef __ANDROID__

#include <utility/android-jni.hpp>

#include <QByteArray>
#include <QJniEnvironment>
#include <QJniObject>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <cstdio>
#include <string.h>

#include <obs.hpp>

/* ---------------- P-18-a：屏幕采集同意总线（libobs ↔ ObsProjectionHost） ---------------- */

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsProjectionHost.java）。
 * 与权限那个类分开的理由写在那文件的头注释里：MediaProjection 没有可查状态，必须真收得到
 * onActivityResult，而 ObsPermissionHost 整套设计的前提恰恰是"不接回调"。 */
static const char kProjectionClass[] = "com/obsproject/studio/ObsProjectionHost";

/* 这里要的是通用 Context 而不是 Activity：起代理 Activity 用的是 startActivity（Context 就有），
 * 不是 requestPermissions（那个才只能在 Activity 上调）。拿不到时传个无效 jobject 下去，
 * Java 侧 ctx 判空 → -1，与相机那两条腿同一个失败形状。
 *
 * 口名是查出来的不是推的：`javap -s` 扫 Qt6Android.jar 里的 org.qtproject.qt.android.QtNative，
 * 静态取上下文的只有两个 —— `activity()` ()Landroid/app/Activity; 与 `getContext()`
 * ()Landroid/content/Context;。**没有 application()**（我第一版按别的框架的习惯写了它，
 * 那样运行时会 NoSuchMethodError → jobject 为空 → 每次 request_consent 都返回 -1，
 * 而这在属性页上看起来与"用户没同意"是同一种表现）。
 * 这两个口都是包内可见，JNI 那侧照样调得到：上面 AndroidJni::activity() 调 activity() 已经是 P-17
 * 实测跑通过的同一条路。 */


/* Java 侧那本账（意图位/令牌/面/最后一次动作与错误）只有它自己能说清，native 侧每一处失败都顺手
 * 带上它 —— 屏幕这条路失败原因全在 Java 里，不带这句就只能靠 logcat 拼。 */
static QString screenDescribe()
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kProjectionClass, "describe",
								 "()Ljava/lang/String;");
	return s.isValid() ? s.toString() : QStringLiteral("describe 调不到（类没进 APK？）");
}

static bool screenHasConsent(void *param)
{
	(void)param;
	return QJniObject::callStaticMethod<jboolean>(kProjectionClass, "hasProjection", "()Z") != 0;
}

static int screenRequestConsent(void *param)
{
	(void)param;
	const QJniObject ctx = AndroidJni::context();
	return (int)QJniObject::callStaticMethod<jint>(kProjectionClass, "requestConsent",
						       "(Landroid/content/Context;)I",
						       ctx.object<jobject>());
}

/* ---------------- P-18-b：把令牌接成一块投影面（量的、接的、收的三条腿） ---------------- */

/* 采集尺寸这一格只做一件事：把 Java 那边 Display.getMode() 的量取结果搬到 native 的 uint32_t 里。
 * 为什么非要绕这一圈而不在前端问 QScreen：QScreen 给的是**逻辑**像素（这台 wm density 覆盖成 240
 * 时 dpr=1.5 ⇒ 1067x667），而 VirtualDisplay 要的是设备物理像素（1280x720），拿错那一档不报错、
 * 只是画面与屏幕不成比例 —— 这类错只能从源头掐掉：源头就是 Java。
 * AndroidJni::context() 拿不到时返回 false（与 request_consent 那格同一个失败形状：问不到就说问不到）。
 *
 * 一次往返拿四个数（d 腿起）：源每秒要问一次尺寸，好发现旋转/分辨率变了。原先那三格
 * captureWidth/Height/Dpi 每次问要跑三遍完整量取，日志也跟着三遍。 */
static bool screenDisplaySize(void *param, uint32_t *width, uint32_t *height, uint32_t *dpi)
{
	(void)param;
	const QJniObject ctx = AndroidJni::context();
	if (!ctx.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸问不到：拿不到 QtNative.getContext()");
		return false;
	}

	const QJniObject arr = QJniObject::callStaticObjectMethod(kProjectionClass, "captureSize",
							   "(Landroid/content/Context;)[I", ctx.object<jobject>());
	if (!arr.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸问不到：captureSize 返回空数组，Java 侧=%s",
		     screenDescribe().toUtf8().constData());
		return false;
	}

	QJniEnvironment env;
	JNIEnv *je = AndroidJni::env(env);
	if (!je) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸读不到：当前线程没有可用的 JNIEnv");
		return false;
	}
	const jintArray ja = arr.object<jintArray>();
	const jsize n = je->GetArrayLength(ja);
	jint *els = (n >= 4) ? (jint *)je->GetIntArrayElements(ja, nullptr) : nullptr;
	if (!els) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸读不到（数组长度=%d，Java 侧=%s）", (int)n,
		     screenDescribe().toUtf8().constData());
		return false;
	}
	const jint w = els[0];
	const jint h = els[1];
	const jint d = els[2];
	je->ReleaseIntArrayElements(ja, els, JNI_ABORT); // 只读，JNI_ABORT = 不把改动写回去

	if (w <= 0 || h <= 0) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 屏幕尺寸不成立 %dx%d（Java 侧=%s）", (int)w, (int)h,
		     screenDescribe().toUtf8().constData());
		return false;
	}

	*width = (uint32_t)w;
	*height = (uint32_t)h;
	if (dpi)
		*dpi = (d > 0) ? (uint32_t)d : 0;
	return true;
}

static bool screenStartDisplay(void *param, void *native_window)
{
	(void)param;
	if (!native_window) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 接面失败：native 传上来的窗口指针是空的");
		return false;
	}
	const QJniObject ctx = AndroidJni::context();
	if (!ctx.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 接面失败：拿不到 QtNative.getContext()");
		return false;
	}

	/* QJniEnvironment 在 Qt 6.9 里**不是模板**、也没有 object()：它是普通类，取 JNIEnv 用
	 * jniEnv() 或 operator->（qjnienvironment.h:17-25）。我按 QtAndroidExtras 时代的旧形状
	 * 写了 `QJniEnvironment<>` 加 `.object()`，编译当场被否 —— 又是"凭记忆写 API 形状"那一笔
	 * （仪器账第 29 条的老根），这回是开着头文件改的。 */
	QJniEnvironment env;
	JNIEnv *je = AndroidJni::env(env);
	if (!je) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 接面失败：当前线程没有可用的 JNIEnv");
		return false;
	}
	ANativeWindow *window = static_cast<ANativeWindow *>(native_window);
	jobject surface = ANativeWindow_toSurface(je, window);
	if (!surface) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: ANativeWindow_toSurface 返回空（窗口=%p）",
		     native_window);
		return false;
	}

	/* 尺寸**取自这块窗口本身**，不再绕回 display_size：AImageReader 是照 display_size 建的，
	 * 窗口几何就是它的尺寸 —— 以传进来的这个对象为唯一真相，两侧就不可能对不上（对不上也只能
	 * 在这一句里暴露出来，而那正是我们想看的）。 */
	const jint w = ANativeWindow_getWidth(window);
	const jint h = ANativeWindow_getHeight(window);
	uint32_t dw = 0, dh = 0, ddpi = 0;
	const jint dpi = screenDisplaySize(nullptr, &dw, &dh, &ddpi) ? (jint)ddpi : 0;

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(
		kProjectionClass, "startCapture", "(Landroid/content/Context;Landroid/view/Surface;III)Z",
		ctx.object<jobject>(), surface, w, h, dpi);

	/* NDK 头只说了"这个 Surface 会自己持有 ANativeWindow 的引用"，**没说这个 jobject 本身是
	 * local 还是 global**（原文见 native_window_jni.h:47-56）。这一格决定要不要还、以及怎么还，
	 * 猜错两半都有代价：把 global 当 local 删是 ART 直接 abort，该删不删是每次 activate 漏一个
	 * 全局引用。所以不猜 —— 拿 GetObjectRefType 现问，并把答案打进日志（1=local 2=global
	 * 3=weak global）：这一轮先看真机给什么，之后再按它收拾。 */
	const jint ref_type = je->GetObjectRefType(surface);
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 接面 %dx%d@%ddpi 结果=%d Surface 引用类型=%d Java 侧=%s",
	     (int)w, (int)h, (int)dpi, (int)ok, (int)ref_type, screenDescribe().toUtf8().constData());

	if (ref_type == JNILocalRefType)
		je->DeleteLocalRef(surface);
	return ok != 0;
}

static void screenStopDisplay(void *param)
{
	(void)param;
	const QJniObject why = QJniObject::fromString(QStringLiteral("native 侧收面（源 deactivate）"));
	QJniObject::callStaticMethod<void>(kProjectionClass, "stopCapture", "(Ljava/lang/String;)V",
					   why.object<jstring>());
}

/* ---------------- P-18-d：把令牌也交回去（第六格） ---------------- */

/* 与上面那格的分别写进 obs-android.h 的结构体注释了，这里只记落地：Java 侧 stopProjection 自己
 * 就按"先收面、再 stop 令牌、再落意图位、再重算前台类型"那一步不落顺序做完，所以这一格不需要
 * 先调 stop_display —— 调两次只是把同一本账翻两遍。它返回 false 表示"手里本来就没有令牌"，
 * 那不是失败，所以这里不 blog 警告，只在真交出去的那一发记一行。 */
static void screenReleaseConsent(void *param)
{
	(void)param;
	const QJniObject why = QJniObject::fromString(QStringLiteral("native 侧交还（最后一颗采集源销毁）"));
	const jboolean did = QJniObject::callStaticMethod<jboolean>(kProjectionClass, "stopProjection",
							    "(Ljava/lang/String;)Z", why.object<jstring>());
	if (did)
		blog(LOG_INFO, "OBSAndroidPermissions[Android]: 投影令牌已交还，Java 侧=%s",
		     screenDescribe().toUtf8().constData());
}

/* ---------------- P-18-e：系统内录三条腿 ---------------- */

/* drainAudio(short[] out, int maxFrames) 一问就是 10 ms 一发（一秒一百次），所以这里的 short[]
 * 养在文件级复用：每次新建就是每秒一百个临时对象（Java 侧那句注释给的是同一个理由）。
 * 存的是 **global ref** —— local ref 只活到本次 JNI 调用结束，而这一格要在插件那条音频线程上
 * 反复用；global ref 不随线程走，所以 start/stop 落在哪条线程都能建和销。
 *
 * 三个静态量各自只被一条路碰：read 在插件的采集线程，start/stop 在视频线程或源销毁路径上。
 * 插件那边是"先把线程 join 掉、再调 stop_audio"（那两行写在 screen-capture.c 的停止那一格），
 * 所以这里不再额外上锁。 */
static jshortArray g_audioScratch = nullptr;
static jsize g_audioScratchShorts = 0;
static uint32_t g_audioScratchChannels = 0;

static void audioReleaseScratch()
{
	if (!g_audioScratch)
		return;
	QJniEnvironment env;
	JNIEnv *je = AndroidJni::env(env);
	if (!je) {
		/* 没有 JNIEnv 时不能碰 global ref：清掉本地账，宁可漏一个 global ref，
		 * 也不在无效 JNIEnv 上解引用。 */
		g_audioScratch = nullptr;
		g_audioScratchShorts = 0;
		g_audioScratchChannels = 0;
		return;
	}
	je->DeleteGlobalRef(g_audioScratch);
	g_audioScratch = nullptr;
	g_audioScratchShorts = 0;
	g_audioScratchChannels = 0;
}

/* Java 侧 startAudio() 把 `{实到采样率, 实到声道数}` 当返回值交出来（不写出参），为的是与
 * captureSize 一样**一趟 JNI 拿全** —— 与屏幕尺寸那格同一个形状，照抄那边的读法。
 * 出参必须是 Java 实到的那一组：AudioRecord 允许把我们要求的格式换成它自己的，报错了就是变速变调。 */
static bool screenStartAudio(void *param, uint32_t *sample_rate, uint32_t *channels)
{
	(void)param;
	/* 先确认当前线程有 JNIEnv，再调用 Java startAudio：把“Java 已经启动 AudioRecord、
	 * 但 native 读不到返回值”的窗口压到零。 */
	QJniEnvironment env;
	JNIEnv *je = AndroidJni::env(env);
	if (!je) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录起不来：当前线程没有可用的 JNIEnv");
		return false;
	}

	const QJniObject arr = QJniObject::callStaticObjectMethod(kProjectionClass, "startAudio", "()[I");
	if (!arr.isValid()) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录起不来（startAudio 返回 null），Java 侧=%s",
		     screenDescribe().toUtf8().constData());
		return false;
	}

	/* startAudio 已经成功（返回了数组）之后，任何失败都必须把 Java 侧 AudioRecord 停掉，
	 * 否则调用方只看到 false，Java 侧的录音线程仍在跑。事务顺序固定为：
	 * 清 pending exception → stopAudio → 释放本次创建的 scratch。 */
	ScreenAudioStartTransaction txn(
		[&] {
			if (je->ExceptionCheck())
				je->ExceptionClear();
		},
		[&] { QJniObject::callStaticMethod<void>(kProjectionClass, "stopAudio", "()V"); },
		[&] { audioReleaseScratch(); });
	txn.arm();

	const jintArray ja = arr.object<jintArray>();
	const jsize n = je->GetArrayLength(ja);
	jint *els = (n >= 2) ? je->GetIntArrayElements(ja, nullptr) : nullptr;
	if (!els) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录格式读不到（数组长度=%d）", (int)n);
		txn.rollback();
		return false;
	}
	const jint rate = els[0];
	const jint ch = els[1];
	je->ReleaseIntArrayElements(ja, els, JNI_ABORT); // 只读，不把改动写回去

	if (rate <= 0 || ch <= 0) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录格式不成立 %dHz×%d，Java 侧=%s", (int)rate,
		     (int)ch, screenDescribe().toUtf8().constData());
		txn.rollback();
		return false;
	}

	/* 缓冲按“一秒”建：48k 立体声 = 96000 个 short = 192 KB，一次性的代价，换来读那一格零分配。
	 * 幂等的第二发（或 Java 侧换了格式）走到这里：容量够就沿用，不够就销旧建新 —— 不销就是每换一次
	 * 漏一枚 global ref。 */
	const jsize need = (jsize)(rate * ch);
	if (g_audioScratch && (g_audioScratchChannels != (uint32_t)ch || g_audioScratchShorts < need))
		audioReleaseScratch();
	if (!g_audioScratch) {
		jshortArray fresh = je->NewShortArray(need);
		if (!fresh) {
			blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录缓冲建不出来（%d 个 short）", (int)need);
			txn.rollback();
			return false;
		}
		g_audioScratch = (jshortArray)je->NewGlobalRef(fresh);
		je->DeleteLocalRef(fresh);
		if (!g_audioScratch) {
			blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录缓冲 global ref 建不出来");
			txn.rollback();
			return false;
		}
		g_audioScratchShorts = need;
		g_audioScratchChannels = (uint32_t)ch;
		txn.markScratchCreated();
	}

	txn.commit();
	if (sample_rate)
		*sample_rate = (uint32_t)rate;
	if (channels)
		*channels = (uint32_t)ch;
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 内录总线已接上 %dHz×%d（复用缓冲 %d 个 short ≈ 1 秒）",
	     (int)rate, (int)ch, (int)g_audioScratchShorts);
	return true;
}

static void screenStopAudio(void *param)
{
	(void)param;
	const bool had = (g_audioScratch != nullptr);
	QJniObject::callStaticMethod<void>(kProjectionClass, "stopAudio", "()V");
	audioReleaseScratch();
	if (had)
		blog(LOG_INFO, "OBSAndroidPermissions[Android]: 内录总线已断开，Java 侧=%s",
		     screenDescribe().toUtf8().constData());
}

/* 取一次 PCM。0 帧 = 此刻没有，不是错误（调用方睡 10 ms 再来）。
 *
 * **顺序要紧**：先 drain 再 GetShortArrayElements。这一格规范上允许给回一份**拷贝**（ART 通常给
 * 直接指针，但不保证），反过来写的症状是"声音永远慢一发"—— 比崩还难查，所以写在注释里。
 * 拿完就 Release(JNI_ABORT)：只读，不回写。 */
static uint32_t screenReadAudio(void *param, int16_t *out, uint32_t max_frames)
{
	(void)param;
	if (!out || max_frames == 0 || !g_audioScratch || g_audioScratchChannels == 0)
		return 0;

	/* max_frames 是**帧**，缓冲的容量是**short**，夹之前先换算（Java 侧 drainAudio 自己也会按
	 * out.length/声道数再夹一道，这里这一道是为了让 jint 那个窄化有界、且把"最多能拿回多少"说清楚）。 */
	const uint32_t cap_frames = (uint32_t)(g_audioScratchShorts / (jsize)g_audioScratchChannels);
	const jint want = (jint)((max_frames < cap_frames) ? max_frames : cap_frames);
	const jint got = QJniObject::callStaticMethod<jint>(kProjectionClass, "drainAudio", "([SI)I",
							    (jobject)g_audioScratch, want);
	if (got <= 0)
		return 0;

	/* 这一条线程是插件自己 pthread_create 出来的原生线程，不在 JVM 的线程表里。
	 * 判 env 有效再解引用：QJniEnvironment 拿不到 JNIEnv 时 jniEnv() 给空指针，
	 * 而 operator-> 就是它 —— 少这一格判空是段错误，不是"读不到"。
	 * （"Qt 会不会替外来线程 attach"这件事不靠读源码断定：屏幕尺寸那一格本来就是 libobs
	 * 的视频线程在每秒问一次、P-18-b/d 两轮的日志里都是真数，外来线程走得通是量出来的。） */
	QJniEnvironment env;
	JNIEnv *je = AndroidJni::env(env);
	if (!je) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录取不到帧：这条线程拿不到 JNIEnv");
		}
		return 0;
	}

	const jsize shorts = (jsize)got * (jsize)g_audioScratchChannels;
	jshort *els = je->GetShortArrayElements(g_audioScratch, nullptr); // 不写 const：Release 那一格入参是 jshort*
	if (!els) {
		blog(LOG_WARNING, "OBSAndroidPermissions[Android]: 内录取到的 short 数组读不到（%d 帧）", (int)got);
		return 0;
	}
	memcpy(out, els, (size_t)shorts * sizeof(int16_t));
	je->ReleaseShortArrayElements(g_audioScratch, els, JNI_ABORT);
	return (uint32_t)got;
}

static void screenCaptureError(void *, char *out, size_t size)
{
	const QJniObject error = QJniObject::callStaticObjectMethod(kProjectionClass, "captureError",
								"()Ljava/lang/String;");
	const QByteArray bytes = error.toString().toUtf8();
	if (size)
		snprintf(out, size, "%s", bytes.constData());
}

static const struct obs_android_screen_host kScreenHost = {
	nullptr,
	screenHasConsent,
	screenRequestConsent,
	screenReleaseConsent,
	screenDisplaySize,
	screenStartDisplay,
	screenStopDisplay,
	screenStartAudio,
	screenStopAudio,
	screenReadAudio,
	screenCaptureError,
};

static bool hardwareDecoderQuery(const char *mime, char *out, size_t size)
{
    auto type = QJniObject::fromString(QString::fromUtf8(mime));
    auto name = QJniObject::callStaticObjectMethod("com/obsproject/studio/ObsCodecCapabilities", "findHardwareDecoder",
        "(Ljava/lang/String;)Ljava/lang/String;", type.object<jstring>());
    QByteArray bytes = name.toString().toUtf8();
    if (bytes.isEmpty() || size <= size_t(bytes.size())) return false;
    memcpy(out, bytes.constData(), size_t(bytes.size()) + 1);
    return true;
}

void obsAndroidPermissionsRegisterScreenBus()
{
	static bool s_bus_registered = false;
	if (s_bus_registered)
		return;
	s_bus_registered = true;

	obs_android_set_screen_host(&kScreenHost);
	obs_android_set_decoder_query(hardwareDecoderQuery);
	obsAndroidScreenGpuHostRegister();

	/* 这一行是 P-18-a/b/d/e 的冒烟抓手。几格在这里**现调**（ready / has_consent / display_size /
	 * start_audio / stop_audio）：本轮还没有任何采集源去问它们，所以"native → JNI → Java 走得通"
	 * 这件事只有靠这一行才有一手证据（打出 ready=1 就说明总线指针对了、JNI 方法名签名对了、类真在 APK 里）。
	 * 注：has_consent 在这里必然读到 =0 —— 令牌不跨进程活，冷启动那一刻手上没有；
	 * true 那一支要等采集源真开起来才能覆。
	 * display_size 不一样：它不要令牌、只要 Context，所以在注册这一刻就能量到真数 ——
	 * 这也是它对不对的第一手证据（预期 1280x720，与 dumpsys display 的 mode 同档）。
	 * start_audio 与 has_consent 同理是**反向**证据：这一刻没有令牌，所以它必须返回 false，
	 * 而 Java 侧会把 lastError 写成"startAudio：没有令牌…"—— 这一句跟着 describe 打出来，
	 * 就同时证明了"调用真到了 Java、方法名与签名都对、判据没错"。万一它返回 true（不该有），
	 * 后面那发 stop_audio 会当场把它收回去。
	 * read_audio 不在这里现调：没有缓冲时它压根不过 JNI，探了也不成为证据。
	 * 另外两格现调不了：request_consent 会在注册那一拍就给用户弹框；start_display 要一块真
	 * AImageReader 的窗口，编不出假的；release_consent 此刻手里没令牌，调了也只是空转。 */
	uint32_t dw = 0, dh = 0, ddpi = 0;
	const bool sized = obs_android_screen_display_size(&dw, &dh, &ddpi);
	uint32_t arate = 0, ach = 0;
	const bool audio_up = obs_android_screen_start_audio(&arate, &ach); // 预期 false（没令牌）
	obs_android_screen_stop_audio();                                    // 上面真起来了的话就地收回；否则空转
	const QJniObject s = QJniObject::callStaticObjectMethod(kProjectionClass, "describe",
								"()Ljava/lang/String;");
	const QString line = QStringLiteral(
				     "屏幕总线九条腿已注册（ready=%1 现读有令牌=%2 尺寸=%3 %4x%5@%6 "
				     "无令牌时内录起=%7 %8Hz×%9）Java 侧=%10")
				     .arg(obs_android_screen_ready() ? 1 : 0)
				     .arg(obs_android_screen_has_consent() ? 1 : 0)
				     .arg(sized ? QStringLiteral("成") : QStringLiteral("败"))
				     .arg(dw)
				     .arg(dh)
				     .arg(ddpi)
				     .arg(audio_up ? QStringLiteral("成（不该）") : QStringLiteral("败（应当）"))
				     .arg(arate)
				     .arg(ach)
				     .arg(s.isValid() ? s.toString()
						      : QStringLiteral("describe 调不到（类没进 APK？）"));
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: %s", line.toUtf8().constData());
}

#endif // __ANDROID__

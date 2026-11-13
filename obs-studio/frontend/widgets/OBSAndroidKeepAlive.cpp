#include "OBSAndroidKeepAlive.hpp"

#ifdef __ANDROID__

#include <utility/android-jni.hpp>

#include <QJniObject>

#include <chrono>
#include <mutex>
#include <cstring>

#include <obs.hpp>

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsForegroundService.java）
 * 全是 public static，和 ObsDisplayHost 一样只走 callStaticMethod。 */
static const char kServiceClass[] = "com/obsproject/studio/ObsForegroundService";

static QString callDescribe()
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kServiceClass, "describe", "()Ljava/lang/String;");
	return s.isValid() ? s.toString() : QString();
}

/* 上一次递给 Java 的状态。前台服务只在"有没有输出在跑"这一维上开关，另加一个状态字符串
 * 只是给通知栏文案去重用的；真正的服务状态以 Java 侧 instance/计数器为准（describe 里能看到）。 */
static std::mutex g_keepAliveMutex;
static bool g_running = false;
static bool g_startPending = false;
static std::chrono::steady_clock::time_point g_startRequestedAt;
static QString g_state;

struct CaptureSources { bool camera = false; bool microphone = false; };

static bool collectCaptureSource(void *data, obs_source_t *source)
{
    auto *sources = static_cast<CaptureSources *>(data);
    if (!obs_source_active(source)) return true;
    const char *id = obs_source_get_id(source);
    if (id && strcmp(id, "android_camera_input") == 0) sources->camera = true;
    if (id && strcmp(id, "android_audio_input") == 0) sources->microphone = true;
    return true;
}

bool obsAndroidKeepAliveSetRunning(bool running, const char *state, QString *detail)
{
    CaptureSources sources;
    obs_enum_sources(collectCaptureSource, &sources);
    const QJniObject activity = AndroidJni::activity();
    const bool capture = activity.isValid() &&
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "setCaptureSources",
            "(Landroid/content/Context;ZZ)Z", activity.object<jobject>(),
            jboolean(sources.camera), jboolean(sources.microphone));
    if (!running && capture) state = "capturing";
    running = running || capture;
	const QString what = state ? QString::fromUtf8(state) : QStringLiteral("running");
	QString jstatus;
	bool repeat = false;
	bool javaReady = false;
	if (running && activity.isValid())
		javaReady = QJniObject::callStaticMethod<jboolean>(kServiceClass, "isForegroundReady", "()Z") != 0;

	{
		std::lock_guard<std::mutex> lk(g_keepAliveMutex);

		/* start() 只是"请求已发出"；真正的前台档在 onStartCommand→startForeground 里。
		 * 在它落定之前保持 pending，不让看门狗每秒重复 start，也不把 g_running 提前写成 true。 */
		if (running && g_startPending) {
			if (javaReady) {
				g_startPending = false;
				g_running = true;
				g_state = what;
				if (detail)
					*detail = QStringLiteral("前台服务已进入前台档(%1)").arg(what);
				return true;
			}
			if (std::chrono::steady_clock::now() - g_startRequestedAt < std::chrono::seconds(5)) {
				if (detail)
					*detail = QStringLiteral("前台服务启动中(%1)，等待 onStartCommand").arg(what);
				return true;
			}
			/* 超时：允许下一次真正的 start() 重试。 */
			g_startPending = false;
		}

		if (!running && !g_running && !g_startPending) {
			if (detail)
				*detail = QStringLiteral("本来就没在跑，忽略 stop");
			return true;
		}
		if (!running && g_startPending) {
			/* stop 与 start 撞车：先清 pending，下面照常发 stopService。 */
			g_startPending = false;
		}
		/* 3-5 看门狗每秒复核一次：状态没变就别再惊动 Java（重复 startForeground 会每秒重刷通知）。
		 * 但如果 Java 侧已经不是前台档（服务被杀/onDestroy），必须重新发起，不能信旧 g_running。 */
		repeat = running && g_running && g_state == what;
		if (repeat && !javaReady)
			g_running = false;
	}

	if (repeat && javaReady) {
		if (detail)
			*detail = QStringLiteral("已在前台档(%1)，忽略重复").arg(what);
		return true;
	}

	const QJniObject act = AndroidJni::activity();
	if (!act.isValid()) {
		if (detail)
			*detail = QStringLiteral("拿不到 QtNative.activity() —— 前台服务没起来");
		jstatus = obsAndroidKeepAliveStatus();
		blog(LOG_WARNING, "OBSAndroidKeepAlive[Android]: %s", jstatus.toUtf8().constData());
		return false;
	}

	bool ok;
	if (running) {
		const QJniObject jwhat = QJniObject::fromString(what);
		ok = QJniObject::callStaticMethod<jboolean>(kServiceClass, "start",
							   "(Landroid/content/Context;Ljava/lang/String;)Z",
							   act.object<jobject>(), jwhat.object<jstring>()) != 0;
	} else {
		ok = QJniObject::callStaticMethod<jboolean>(kServiceClass, "stop", "(Landroid/content/Context;)Z",
							    act.object<jobject>()) != 0;
	}

	/* 成功之后才提交状态。失败时保留旧值，看门狗下一拍会重试 —— 先把 g_running 写成
	 * 目标值再调 Java 的话，失败的那次会被下一拍判成"已在跑"并直接短路，结果是一次
	 * startForeground 失败（通知权限被拒、OEM 限制）就永久没有前台服务，推流/录制
	 * 可以在后台被系统杀掉而不再有任何重试。 */
	if (ok) {
		std::lock_guard<std::mutex> lk(g_keepAliveMutex);
		if (running) {
			g_startPending = true;
			g_startRequestedAt = std::chrono::steady_clock::now();
			g_running = false; /* 等 isForegroundReady() 确认 */
		} else {
			g_startPending = false;
			g_running = false;
		}
		g_state = what;
	}

	jstatus = obsAndroidKeepAliveStatus();
	const QString verb = running ? QStringLiteral("起前台服务(%1)").arg(what) : QStringLiteral("停前台服务");
	const QString line = QStringLiteral("%1 %2：%3")
				     .arg(verb)
				     .arg(ok ? (running ? QStringLiteral("已发起") : QStringLiteral("成功")) : QStringLiteral("失败"))
				     .arg(jstatus);
	if (detail)
		*detail = line;
	blog(ok ? LOG_INFO : LOG_WARNING, "OBSAndroidKeepAlive[Android]: %s", line.toUtf8().constData());
	return ok;
}

QString obsAndroidKeepAliveStatus()
{
	const QString desc = callDescribe();
	if (!desc.isEmpty())
		return desc;
	/* describe() 调不到（类没进 APK / 静态方法没搜到）时至少把 native 侧的意图报出来，
	 * 与"服务在跑但 describe 返回空"是两回事。 */
	return QStringLiteral("describe 空；native 侧期望 running=%1").arg(g_running);
}

#endif // __ANDROID__

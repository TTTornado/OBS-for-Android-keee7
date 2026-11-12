#include "OBSAndroidPermissions.hpp"

#ifdef __ANDROID__

#include <utility/android-jni.hpp>

#include <QGuiApplication>
#include <QObject>

#include <atomic>

#include <obs.hpp>

static const char kPermissionClass[] = "com/obsproject/studio/ObsPermissionHost";

/* ---------------- P-17：相机权限总线（libobs ↔ ObsPermissionHost） ---------------- */

/* 为什么这一格必须由前端来填：插件 .so 只链 libobs、没有 JavaVM，而 CAMERA 授予态只有 Java 的
 * Context 问得到（Camera2 NDK 里没有 checkSelfPermission）。总线本体在 libobs/obs-android.h，
 * 前端在这里把两条腿接上。
 *
 * 两条腿都把 Activity 当 Context 传：checkSelfPermission 是 Context 的方法，用 Activity 调合法；
 * 而 requestCamera 里的 requestPermissions/runOnUiThread 只能在 Activity 上有，所以不能退到
 * AndroidJni::context()。拿不到 Activity 时两处都按失败走（Java 侧 ctx/act 判空 → false / -1），
 * 不会抛 JNI 异常 —— 这也是这张表可以是文件级 const、退出时不忙着注销的原因。 */
static bool cameraHasPermission(void *param)
{
	(void)param;
	const QJniObject act = AndroidJni::activity();
	return QJniObject::callStaticMethod<jboolean>(kPermissionClass, "hasCameraPermission",
						      "(Landroid/content/Context;)Z",
						      act.object<jobject>()) != 0;
}

static int cameraRequestPermission(void *param)
{
	(void)param;
	const QJniObject act = AndroidJni::activity();
	return (int)QJniObject::callStaticMethod<jint>(kPermissionClass, "requestCamera",
						       "(Landroid/app/Activity;)I",
						       act.object<jobject>());
}

/* ---------------- P-17-f：前后台 → "此刻该不该占着相机" ---------------- */

/* 为什么这一格也要前端来填（跟权限那两条不是一个理由）：Camera2 的 Activity 生命周期只有
 * Java/Qt 那侧看得见 —— 插件是纯 C、没有 Activity 回调，而 Android 的政策是"你的窗口停了
 * 就该把相机放下"，不放的代价是下次进应用直接 ACAMERA_ERROR_CAMERA_IN_USE。
 *
 * 为什么**缓存**而不是一问就去读 applicationState()：这条腿是在**视频线程**上被 camera2_video_tick
 * 读的，而 QGuiApplication::applicationState()（Qt 6.9.3，android_x86_64/include/QtGui/
 * qguiapplication.h:126，信号在同文件 :153）在 Qt 里属于平台插件的 GUI 线程状态。
 * 所以只在 UI 线程收到 applicationStateChanged 时写一次这个原子量，别的线程只读。 */
static std::atomic<bool> g_captureAllowed{true};

static bool cameraCaptureAllowed(void *param)
{
	(void)param;
	return g_captureAllowed.load();
}

/* 交还的判据是"面停了"= Hidden/Suspended（Android 的 onStop），**不含 Inactive**（onPause 那一格）。
 *
 * 这条是量出来的，不是我推的：b107 原本写的是"只有 Active 才算该持有"，理由是"两边后果不对称，
 * 取保守那一头" —— 这个理由漏了一步：**保守只有在"暂时的状态之后一定还会再来一次真信号"时才成立**。
 * 实测它不成立：19:38:06 一个 monkey 发的 LAUNCH_SINGLE_TOP 重启意图让 Qt 连着发了
 * state=4 → state=2，然后就**停**在 2；同一时刻 dumpsys 说 topResumedActivity=QtActivity、
 * state=RESUMED、mCurrentFocus 也还是我们，进程 16293 活着，日志之后 109 秒再没一行。
 * 后果从"保守"变成了"永久没画面"：相机被交还，而 g_capture_allowed 再没有翻回 true，
 * 插件 f 腿第 4 支那个重开条件（allowed）永远不满足 —— 用户看到的是一个不报错的黑源。
 *
 * 换成现在的判据，同一趟生命周期各状态的落点（MuMu 实测）：HOME → 2 然后 0（交还照旧发生，
 * 立案时那条硬约束②"退后台不交还设备就是 CAMERA_IN_USE"仍然满足）；对话框/下拉栏/重启意图抢
 * 焦点 → 停在 2（画面不断）；回前台 → 4（重开，实测 11 ms）。
 *
 * 单独提成一个函数、不写两遍：下面冷启动起头那一处也用它，判据只有一份。 */
static bool stateAllowsCapture(Qt::ApplicationState state)
{
	return (state == Qt::ApplicationActive || state == Qt::ApplicationInactive);
}

static void onAndroidApplicationChanged(Qt::ApplicationState state)
{
	const bool allowed = stateAllowsCapture(state);
	const bool was = g_captureAllowed.exchange(allowed);
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 应用态变了 state=%d（Qt6 qnamespace.h:271-276："
		       "0=Suspended 1=Hidden 2=Inactive 4=Active），可采集 %d→%d%s",
	     (int) state, (int) was, (int) allowed, was == allowed ? "（这一格没变）" : "");
}

static const struct obs_android_camera_host kCameraHost = {
	nullptr,
	cameraHasPermission,
	cameraRequestPermission,
	/* 第三条腿：位置对应 struct 里的 capture_allowed（照字段顺序写，与上面两条同一个形状）。
	 * 少了这一格就是 NULL，而 obs_android_camera_capture_allowed() 对 NULL 是 fail-open
	 * （返回 true）—— 忘了接的后果不是编不过，是"退后台不交还设备"，
	 * 而那要到真机上才看得见。 */
	cameraCaptureAllowed,
};

void obsAndroidPermissionsRegisterCameraBus()
{
	/* 头里那句"注册是幂等的"靠这个 bool 兜住：总线本身是覆盖写，重复注册没事，
	 * 但下面那次 connect 会**累加** —— 连接归 sender（App，活到进程结束）所有，
	 * 没有 context 对象替我们解绑，所以只允许接一次。 */
	static bool s_bus_registered = false;
	if (s_bus_registered)
		return;
	s_bus_registered = true;

	/* 先按"现在这一刻"的态起头，再挂信号：注册发生在建窗口之后（OBSBasic 构造里），
	 * 那时可能还没 Active —— b107 用旧判据（只认 Active）就在这儿起过冲突：冷启动那一拍
	 * 量到"可采集初值=0"，源 activate 后刚开起来 139 ms 就被自己拆掉（19:36:34.352 开、
	 * .492 拆），等 .525 的 state=4 再重开。用同一个判据之后 Inactive 也算可持有，
	 * 这一趟开→拆→开没了；万一将来又在建窗口之前注册，这一句也让第一拍就是对的。 */
	g_captureAllowed.store(stateAllowsCapture(QGuiApplication::applicationState()));
	/* 两个坑，都是 b107 编出来的、不是推定：
	 *  ① Qt 6 没有全局 connect()（不加限定名只在 QObject 派生类的成员函数里捡得到，
	 *     本文件这个是自由函数）⇒ 写 QObject::connect，与 dialogs/OBSAbout.cpp:71 同形。
	 *  ② 三连参那个"无 context"重载在这一版里根本不存在：前端这个 TU 的 DEFINES 里有
	 *     QT_ENABLE_STRICT_MODE_UP_TO=0xFF0000（build-fe-x86_64-plugins/build.ninja），
	 *     而 strict mode ≥ 6.7.0 就把 QT_NO_CONTEXTLESS_CONNECT 定义出来
	 *     （QtCore/qtconfigmacros.h:211-215），于是 qobject.h:275-283 那整个重载被 #ifndef 掉
	 *     —— 报的是 "no matching function for call to 'connect'"。⇒ 必须显式给 context。
	 * context 给 qGuiApp 本身：App 活到进程结束，这条连接就跟着它活到进程结束，
	 * 与上面那个 bool 记的"只注册一次"是同一笔账。形状抄 frontend/OBSApp.cpp:1681-1682。
	 * 用 qGuiApp 而不是 qApp：qApp 每装一个模块重定义一次（QtCore/QtGui/QtWidgets 各一份），
	 * 静态类型取决于本次编译最后展开了哪个头；qGuiApp 只在 qguiapplication.h:34 定义，就是 QGuiApplication*。 */
	QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, qGuiApp,
			 &onAndroidApplicationChanged);

	obs_android_set_camera_host(&kCameraHost);
	blog(LOG_INFO, "OBSAndroidPermissions[Android]: 相机总线三条腿已注册（可采集初值=%d）",
	     (int) g_captureAllowed.load());
}


#endif // __ANDROID__

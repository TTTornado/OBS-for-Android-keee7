#include "OBSAndroidScreenGpuHost.hpp"

#ifdef __ANDROID__

#include <utility/android-jni.hpp>

#include <QJniEnvironment>
#include <QJniObject>

#include <obs.hpp>

static constexpr auto kScreenGpuClass = "com/obsproject/studio/ObsScreenGpuHost";
struct ScreenGpuHandle { QJniObject object; };
static void *screenGpuCreate(uint32_t texture, uint32_t width, uint32_t height, uint32_t dpi)
{
 const QJniObject context = AndroidJni::context();
 if (!context.isValid()) return nullptr;
 auto object = QJniObject::callStaticObjectMethod(kScreenGpuClass, "create",
  "(Landroid/content/Context;IIII)Lcom/obsproject/studio/ObsScreenGpuHost;",
  context.object<jobject>(), jint(texture), jint(width), jint(height), jint(dpi));
 if (!object.isValid()) return nullptr;
 return new ScreenGpuHandle{object};
}
static int screenGpuUpdate(void *ptr, float matrix[16])
{
 auto *h = static_cast<ScreenGpuHandle *>(ptr);
 QJniEnvironment env;
 JNIEnv *je = AndroidJni::env(env);
 if (!je) return -1;
 jfloatArray a = je->NewFloatArray(16);
 if (!a) return -1;
 int result = h->object.callMethod<jint>("update", "([F)I", a);
 if (result == 1) je->GetFloatArrayRegion(a, 0, 16, matrix);
 je->DeleteLocalRef(a);
 return result;
}
static void screenGpuDestroy(void *ptr)
{
 auto *h = static_cast<ScreenGpuHandle *>(ptr);
 h->object.callMethod<void>("close");
 delete h;
}
static const struct obs_android_screen_gpu_host kScreenGpuHost = {screenGpuCreate, screenGpuUpdate, screenGpuDestroy};

void obsAndroidScreenGpuHostRegister()
{
	obs_android_set_screen_gpu_host(&kScreenGpuHost);
}

#endif // __ANDROID__

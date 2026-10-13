// librssetup.so - the setup program's installer (tools/setup) for the Android
// launcher (2026-10-13): finds the game, its title update and the add-ons in
// a folder the player picks, and lays them out in /sdcard/RealSteel the way
// the PC setup programs do. The launcher calls it on a background thread.
#include <jni.h>

#include <string>

#include "../../tools/setup/install.h"

namespace {

std::string ToString(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out = chars != nullptr ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
  return out;
}

}  // namespace

extern "C" {

// What a folder holds, one "key=value" per line: game, update (descriptions,
// empty when missing), addons (count), looked (files looked at), other
// (packages of other games).
JNIEXPORT jstring JNICALL Java_com_recompiledsteel_app_Installer_scan(JNIEnv* env, jclass,
                                                                      jstring folder) {
  const rs::setup::Scan scan = rs::setup::ScanFolder(ToString(env, folder));
  std::string out;
  out += "game=" + (scan.game.empty() ? std::string() : rs::setup::Describe(scan.game)) + "\n";
  out += "update=" + (scan.update.empty() ? std::string() : rs::setup::Describe(scan.update)) + "\n";
  out += "addons=" + std::to_string(scan.addons.size()) + "\n";
  out += "looked=" + std::to_string(scan.files_looked_at) + "\n";
  out += "other=" + std::to_string(scan.other_titles) + "\n";
  return env->NewStringUTF(out.c_str());
}

// Installs everything found in |folder| into |dest| (game/, tu/) and
// |user_root| (add-ons). |listener| gets onProgress(double, String) and
// onLog(String). Returns "" on success, else the reason it stopped.
JNIEXPORT jstring JNICALL Java_com_recompiledsteel_app_Installer_install(JNIEnv* env, jclass,
                                                                         jstring folder,
                                                                         jstring dest,
                                                                         jstring user_root,
                                                                         jobject listener) {
  const rs::setup::Scan scan = rs::setup::ScanFolder(ToString(env, folder));
  rs::setup::Plan plan;
  plan.game = scan.game;
  plan.update = scan.update;
  plan.addons = scan.addons;
  plan.dest = ToString(env, dest);
  plan.user_root = ToString(env, user_root);
  if (plan.game.empty()) {
    return env->NewStringUTF("The game package was not found in that folder.");
  }

  jclass cls = env->GetObjectClass(listener);
  jmethodID on_progress = env->GetMethodID(cls, "onProgress", "(DLjava/lang/String;)V");
  jmethodID on_log = env->GetMethodID(cls, "onLog", "(Ljava/lang/String;)V");
  auto log = [&](const std::string& line) {
    jstring text = env->NewStringUTF(line.c_str());
    env->CallVoidMethod(listener, on_log, text);
    env->DeleteLocalRef(text);
  };
  double last = -1.0;
  auto progress = [&](double fraction, const std::string& what) {
    if (fraction - last < 0.002 && fraction < 1.0) return;  // the UI needs no more
    last = fraction;
    jstring text = env->NewStringUTF(what.c_str());
    env->CallVoidMethod(listener, on_progress, fraction, text);
    env->DeleteLocalRef(text);
  };
  std::string error;
  const bool ok = rs::setup::Run(plan, log, progress, &error);
  return env->NewStringUTF(ok ? "" : (error.empty() ? "The install failed." : error.c_str()));
}

}  // extern "C"

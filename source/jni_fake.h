/* JNI compatibility environment for Unity and the game libraries.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to finish
extern volatile int jni_quit_requested;

typedef void (*jni_snapshot_opened_fn)(void *env, void *clazz,
                                       int32_t request_id, void *result);
typedef void (*jni_snapshot_committed_fn)(void *env, void *clazz,
                                          int32_t request_id, void *result);

void jni_init(void);
void jni_process_soft_keyboard(void);
void jni_set_snapshot_opened_callback(jni_snapshot_opened_fn callback);
void jni_set_snapshot_committed_callback(jni_snapshot_committed_fn callback);
/* Returns non-zero when an offline snapshot callback was delivered. */
void jni_process_deferred_callbacks(void);

// the fake MyNativeActivity jobject handed to ANativeActivity.clazz
void *jni_make_activity_object(void);

// fake Java object / string constructors
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

// Stable Android-style locale name (for example "fr_FR") selected from libnx.
const char *jni_locale_name(void);

#endif

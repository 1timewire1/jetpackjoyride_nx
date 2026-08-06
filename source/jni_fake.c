/* jni_fake.c -- Android/JNI compatibility environment.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "unity_jni.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_BUNDLE = 0x424e4431, // 'BND1'
  TAG_SNAP_OPEN = 0x534f5031, // 'SOP1'
  TAG_SNAP_COMMIT = 0x53434d31, // 'SCM1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

#define MAX_BUNDLE_KEYS 512
#define MAX_STORE_PRODUCT_ID 128
typedef struct {
  uint32_t tag;
  unsigned count;
  char keys[MAX_BUNDLE_KEYS][MAX_STORE_PRODUCT_ID];
} FakeBundle;

typedef struct {
  uint32_t tag;
  int success;
  int data_len;
  uint8_t *data;
  void *snapshot;
} FakeSnapshotOpenResult;

typedef struct {
  uint32_t tag;
  int success;
} FakeSnapshotCommitResult;

volatile int jni_quit_requested = 0;

typedef void (*UnityKeyboardVisibleFn)(void *, void *, uint8_t);
typedef void (*UnityInputStringFn)(void *, void *, void *);
typedef void (*UnitySoftInputFn)(void *, void *);
typedef void (*MortarNativeThreadEntryFn)(void *, void *, int64_t);
typedef void (*GooglePlayProductsInfoFn)(void *, void *, void *, void *, void *,
                                         void *, void *, void *, int64_t);
typedef void (*GooglePlayPurchaseResultFn)(void *, void *, void *, void *,
                                           uint8_t, uint8_t, void *);
typedef void (*GooglePlayPendingEndedFn)(void *, void *);
typedef void (*GooglePlayRestoreCompletedFn)(void *, void *, void *);

static UnityKeyboardVisibleFn g_unity_keyboard_visible;
static UnityInputStringFn g_unity_input_string;
static UnitySoftInputFn g_unity_soft_input_closed;
static UnitySoftInputFn g_unity_soft_input_canceled;
static MortarNativeThreadEntryFn g_mortar_native_thread_entry;

static struct {
  Mutex lock;
  int pending;
  int keyboard_type;
  int secure;
  int character_limit;
  char initial[0x1001];
  char guide[0x101];
} g_soft_keyboard;

/* Snapshot callbacks are delivered after the request method returns. */
#define MAX_DEFERRED_SNAPSHOT_OPENS 32
#define MAX_DEFERRED_SNAPSHOT_COMMITS 32
static struct {
  Mutex lock;
  jni_snapshot_opened_fn opened;
  jni_snapshot_committed_fn committed;
  int next_request_id;
  unsigned open_head;
  unsigned open_count;
  int open_request_ids[MAX_DEFERRED_SNAPSHOT_OPENS];
  unsigned commit_head;
  unsigned commit_count;
  int commit_request_ids[MAX_DEFERRED_SNAPSHOT_COMMITS];
  uint8_t commit_success[MAX_DEFERRED_SNAPSHOT_COMMITS];
  int last_write_ok;
} g_snapshot_callbacks;

/* Offline billing keeps Mortar's asynchronous callback contract. */
#define MAX_PRODUCT_INFO_REQUESTS 4
#define MAX_DEFERRED_PURCHASES 32
typedef struct {
  int64_t request_id;
  unsigned product_count;
  char product_ids[MAX_BUNDLE_KEYS][MAX_STORE_PRODUCT_ID];
} ProductInfoRequest;

static struct {
  Mutex lock;
  GooglePlayProductsInfoFn products_info;
  GooglePlayPurchaseResultFn purchase_result;
  GooglePlayPendingEndedFn pending_ended;
  GooglePlayRestoreCompletedFn restore_completed;

  unsigned info_head, info_count;
  ProductInfoRequest info[MAX_PRODUCT_INFO_REQUESTS];

  unsigned purchase_head, purchase_count;
  char purchase_ids[MAX_DEFERRED_PURCHASES][MAX_STORE_PRODUCT_ID];
  uint64_t next_token;

  unsigned pending_end_count;
  unsigned restore_complete_count;
} g_store_callbacks;

// ---------------------------------------------------------------------------
// local reference registry (matches the engine's Push/PopLocalFrame brackets)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// interned-string pool: the engine re-creates the same constant strings (class
// names, the activity name) constantly; pool them by content so repeats don't
// fill the local-ref table. Pooled strings are never reg_local'd, and free_ref
// skips them (range check below).
#define MAX_ISTR 512
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static void free_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;  // interned string -- pooled, never freed
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_BUNDLE: free(ref); break;
    case TAG_SNAP_OPEN: {
      FakeSnapshotOpenResult *r = ref;
      free(r->data);
      free(r);
      break;
    }
    case TAG_SNAP_COMMIT: free(ref); break;
    case TAG_OBJECT: free(ref); break;
    /* Opaque framework handles are shared and remain valid for the process. */
    default: break; // TAG_ID / TAG_CLASS / UJ_TAG are pooled or externally owned
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Intern objects by label -- one pooled object per class (TAG_CLASS so free_ref()
// leaves it alone, never reg_local'd) -- so the engine's frequent NewObject calls
// don't fill the local-ref table. Safe: our objects are opaque, stateless handles
// dispatched by method class, not by identity.
#define MAX_IOBJ 512
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&locals_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (istr_count < MAX_ISTR) {
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->utf = strdup(u);
    mutexUnlock(&locals_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&locals_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  s->tag = TAG_STRING;
  s->utf = strdup(u);
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

/* Google Play snapshots are the game's authoritative profile store.  The Java
 * Billing/Games SDK normally owns these bytes; without it, returning a fresh
 * empty snapshot on every boot discards profile progress and entitlements.
 * Persist the exact encrypted payload Mortar gives WriteSnapshot(), without
 * interpreting or modifying game data. */
#define LOCAL_SNAPSHOT_PATH GAME_HOME "/save/google_play_snapshot.bin"
#define LOCAL_SNAPSHOT_TEMP GAME_HOME "/save/google_play_snapshot.bin.tmp"
#define LOCAL_SNAPSHOT_BACKUP GAME_HOME "/save/google_play_snapshot.bin.bak"
#define MAX_LOCAL_SNAPSHOT_BYTES (16 * 1024 * 1024)

static int local_snapshot_read(uint8_t **data_out, int *len_out) {
  *data_out = NULL;
  *len_out = 0;
  FILE *file = fopen(LOCAL_SNAPSHOT_PATH, "rb");
  if (!file) {
    const int error = errno;
    return error == ENOENT ? 0 : -1;
  }

  int ok = fseek(file, 0, SEEK_END) == 0;
  long size = ok ? ftell(file) : -1;
  if (size < 0 || size > MAX_LOCAL_SNAPSHOT_BYTES ||
      (size > 0 && fseek(file, 0, SEEK_SET) != 0)) {
    fclose(file);
    return -1;
  }

  uint8_t *data = NULL;
  if (size > 0) {
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
      free(data);
      fclose(file);
      return -1;
    }
  }
  if (fclose(file) != 0) {
    free(data);
    return -1;
  }
  *data_out = data;
  *len_out = (int)size;
  return 1;
}

static int local_snapshot_write(const void *data, int len) {
  if (len < 0 || len > MAX_LOCAL_SNAPSHOT_BYTES || (len && !data))
    return 0;
  if (mkdir(GAME_HOME "/save", 0777) != 0 && errno != EEXIST)
    return 0;

  remove(LOCAL_SNAPSHOT_TEMP);
  FILE *file = fopen(LOCAL_SNAPSHOT_TEMP, "wb");
  if (!file) return 0;
  int ok = (!len || fwrite(data, 1, (size_t)len, file) == (size_t)len) &&
           !ferror(file) && fflush(file) == 0;
  if (ok && fsync(fileno(file)) != 0 && errno != ENOSYS && errno != EINVAL)
    ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    remove(LOCAL_SNAPSHOT_TEMP);
    return 0;
  }

  remove(LOCAL_SNAPSHOT_BACKUP);
  const int had_old = rename(LOCAL_SNAPSHOT_PATH, LOCAL_SNAPSHOT_BACKUP) == 0;
  if (!had_old && errno != ENOENT) {
    remove(LOCAL_SNAPSHOT_TEMP);
    return 0;
  }
  if (rename(LOCAL_SNAPSHOT_TEMP, LOCAL_SNAPSHOT_PATH) != 0) {
    if (had_old) rename(LOCAL_SNAPSHOT_BACKUP, LOCAL_SNAPSHOT_PATH);
    remove(LOCAL_SNAPSHOT_TEMP);
    return 0;
  }
  if (had_old) remove(LOCAL_SNAPSHOT_BACKUP);
  return 1;
}

/* Mortar keeps privacy/age consent separately from its Google Play snapshot.
 * JNIWrapper::KeyStoreWrapper forwards those values to the Java KeyStore class,
 * which is absent on Switch.  Preserve that small string store verbatim and
 * synchronously: UserDataConsent writes UserConsentVersion only after the age
 * screen is accepted, and it must survive even if Homebrew Menu later kills the
 * process without delivering Android's normal pause/stop lifecycle. */
#define LOCAL_KEYSTORE_PATH GAME_HOME "/save/keystore.bin"
#define LOCAL_KEYSTORE_TEMP GAME_HOME "/save/keystore.bin.tmp"
#define LOCAL_KEYSTORE_BACKUP GAME_HOME "/save/keystore.bin.bak"
#define LOCAL_KEYSTORE_MAGIC UINT32_C(0x31534b4a) /* "JKS1" */
#define LOCAL_KEYSTORE_VERSION UINT32_C(1)
#define MAX_LOCAL_KEYSTORE_ENTRIES 256
#define MAX_LOCAL_KEYSTORE_KEY_BYTES 255
#define MAX_LOCAL_KEYSTORE_VALUE_BYTES 4095

typedef struct {
  char key[MAX_LOCAL_KEYSTORE_KEY_BYTES + 1];
  char value[MAX_LOCAL_KEYSTORE_VALUE_BYTES + 1];
} LocalKeyStoreEntry;

static struct {
  Mutex lock;
  int loaded;
  unsigned count;
  LocalKeyStoreEntry entries[MAX_LOCAL_KEYSTORE_ENTRIES];
} g_local_keystore;

/* Called with g_local_keystore.lock held.  A return of zero means no file;
 * negative means an unreadable/corrupt file; positive means success. */
static int local_keystore_read_file_locked(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return errno == ENOENT ? 0 : -1;

  uint32_t header[4] = {0};
  int ok = fread(header, sizeof header[0], 4, file) == 4 &&
           header[0] == LOCAL_KEYSTORE_MAGIC &&
           header[1] == LOCAL_KEYSTORE_VERSION &&
           header[2] <= MAX_LOCAL_KEYSTORE_ENTRIES;
  unsigned count = ok ? (unsigned)header[2] : 0;

  for (unsigned i = 0; ok && i < count; ++i) {
    uint16_t lengths[2] = {0};
    ok = fread(lengths, sizeof lengths[0], 2, file) == 2 &&
         lengths[0] > 0 && lengths[0] <= MAX_LOCAL_KEYSTORE_KEY_BYTES &&
         lengths[1] <= MAX_LOCAL_KEYSTORE_VALUE_BYTES;
    if (!ok) break;
    LocalKeyStoreEntry *entry = &g_local_keystore.entries[i];
    ok = fread(entry->key, 1, lengths[0], file) == lengths[0] &&
         fread(entry->value, 1, lengths[1], file) == lengths[1];
    if (!ok) break;
    entry->key[lengths[0]] = '\0';
    entry->value[lengths[1]] = '\0';
  }

  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    g_local_keystore.count = 0;
    return -1;
  }
  g_local_keystore.count = count;
  return 1;
}

static void local_keystore_load_locked(void) {
  if (g_local_keystore.loaded) return;
  g_local_keystore.loaded = 1;
  g_local_keystore.count = 0;

  int loaded = local_keystore_read_file_locked(LOCAL_KEYSTORE_PATH);
  if (loaded <= 0) {
    const int primary_result = loaded;
    g_local_keystore.count = 0;
    loaded = local_keystore_read_file_locked(LOCAL_KEYSTORE_BACKUP);
    if (loaded > 0) return;
    g_local_keystore.count = 0;
    (void)primary_result;
    return;
  }
}

/* Called with g_local_keystore.lock held. */
static int local_keystore_flush_locked(void) {
  if (mkdir(GAME_HOME "/save", 0777) != 0 && errno != EEXIST)
    return 0;

  remove(LOCAL_KEYSTORE_TEMP);
  FILE *file = fopen(LOCAL_KEYSTORE_TEMP, "wb");
  if (!file) return 0;

  const uint32_t header[4] = {
    LOCAL_KEYSTORE_MAGIC, LOCAL_KEYSTORE_VERSION,
    (uint32_t)g_local_keystore.count, 0
  };
  int ok = fwrite(header, sizeof header[0], 4, file) == 4;
  for (unsigned i = 0; ok && i < g_local_keystore.count; ++i) {
    const LocalKeyStoreEntry *entry = &g_local_keystore.entries[i];
    const uint16_t lengths[2] = {
      (uint16_t)strlen(entry->key), (uint16_t)strlen(entry->value)
    };
    ok = fwrite(lengths, sizeof lengths[0], 2, file) == 2 &&
         fwrite(entry->key, 1, lengths[0], file) == lengths[0] &&
         fwrite(entry->value, 1, lengths[1], file) == lengths[1];
  }
  ok = ok && !ferror(file) && fflush(file) == 0;
  if (ok && fsync(fileno(file)) != 0 && errno != ENOSYS && errno != EINVAL)
    ok = 0;
  if (fclose(file) != 0) ok = 0;
  if (!ok) {
    remove(LOCAL_KEYSTORE_TEMP);
    return 0;
  }

  if (remove(LOCAL_KEYSTORE_BACKUP) != 0 && errno != ENOENT) {
    remove(LOCAL_KEYSTORE_TEMP);
    return 0;
  }
  const int had_old = rename(LOCAL_KEYSTORE_PATH, LOCAL_KEYSTORE_BACKUP) == 0;
  if (!had_old && errno != ENOENT) {
    remove(LOCAL_KEYSTORE_TEMP);
    return 0;
  }
  if (rename(LOCAL_KEYSTORE_TEMP, LOCAL_KEYSTORE_PATH) != 0) {
    if (had_old) rename(LOCAL_KEYSTORE_BACKUP, LOCAL_KEYSTORE_PATH);
    remove(LOCAL_KEYSTORE_TEMP);
    return 0;
  }
  if (had_old) remove(LOCAL_KEYSTORE_BACKUP);
  return 1;
}

static int local_keystore_valid_input(const char *key, const char *value) {
  if (!key || !value || !key[0] ||
      strnlen(key, MAX_LOCAL_KEYSTORE_KEY_BYTES + 1) >
          MAX_LOCAL_KEYSTORE_KEY_BYTES ||
      strnlen(value, MAX_LOCAL_KEYSTORE_VALUE_BYTES + 1) >
          MAX_LOCAL_KEYSTORE_VALUE_BYTES)
    return 0;
  return 1;
}

static int local_keystore_find_locked(const char *key) {
  for (unsigned i = 0; i < g_local_keystore.count; ++i)
    if (!strcmp(g_local_keystore.entries[i].key, key)) return (int)i;
  return -1;
}

/* Called with g_local_keystore.lock held and validated strings. */
static int local_keystore_store_locked(const char *key, const char *value) {
  int index = local_keystore_find_locked(key);
  const int was_new = index < 0;
  if (was_new) {
    if (g_local_keystore.count >= MAX_LOCAL_KEYSTORE_ENTRIES)
      return 0;
    index = (int)g_local_keystore.count++;
  }

  LocalKeyStoreEntry *entry = &g_local_keystore.entries[index];
  char previous[MAX_LOCAL_KEYSTORE_VALUE_BYTES + 1];
  if (!was_new) snprintf(previous, sizeof previous, "%s", entry->value);
  snprintf(entry->key, sizeof entry->key, "%s", key);
  snprintf(entry->value, sizeof entry->value, "%s", value);

  if (local_keystore_flush_locked()) return 1;
  if (was_new) {
    memset(entry, 0, sizeof *entry);
    --g_local_keystore.count;
  } else {
    snprintf(entry->value, sizeof entry->value, "%s", previous);
  }
  return 0;
}

static char *local_keystore_get_copy(const char *key) {
  char *value = NULL;
  if (!key || !key[0] ||
      strnlen(key, MAX_LOCAL_KEYSTORE_KEY_BYTES + 1) >
          MAX_LOCAL_KEYSTORE_KEY_BYTES)
    return NULL;

  mutexLock(&g_local_keystore.lock);
  local_keystore_load_locked();
  const int index = local_keystore_find_locked(key);
  if (index >= 0) value = strdup(g_local_keystore.entries[index].value);
  mutexUnlock(&g_local_keystore.lock);
  return value;
}

static int local_keystore_set_value(const char *key, const char *value) {
  if (!local_keystore_valid_input(key, value)) return 0;
  mutexLock(&g_local_keystore.lock);
  local_keystore_load_locked();
  const int ok = local_keystore_store_locked(key, value);
  mutexUnlock(&g_local_keystore.lock);
  return ok;
}

static int local_keystore_set_value_if(const char *key, const char *value,
                                       const char *expected) {
  if (!local_keystore_valid_input(key, value) || !expected ||
      strnlen(expected, MAX_LOCAL_KEYSTORE_VALUE_BYTES + 1) >
          MAX_LOCAL_KEYSTORE_VALUE_BYTES)
    return 0;
  mutexLock(&g_local_keystore.lock);
  local_keystore_load_locked();
  const int index = local_keystore_find_locked(key);
  const int matches = index >= 0
      ? !strcmp(g_local_keystore.entries[index].value, expected)
      : expected[0] == '\0';
  const int ok = matches && local_keystore_store_locked(key, value);
  mutexUnlock(&g_local_keystore.lock);
  return ok;
}

static void *make_snapshot_open_result(void) {
  FakeSnapshotOpenResult *result = calloc(1, sizeof(*result));
  if (!result)
    return jni_make_object(
        "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotOpenResult");
  result->tag = TAG_SNAP_OPEN;
  result->success = 1;
  const int loaded = local_snapshot_read(&result->data, &result->data_len);
  if (loaded > 0) {
    result->snapshot =
        jni_make_object("com/google/android/gms/games/snapshot/Snapshot");
  }
  return reg_local(result);
}

static void *make_snapshot_commit_result(int success) {
  FakeSnapshotCommitResult *result = calloc(1, sizeof(*result));
  if (!result)
    return jni_make_object(
        "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotCommitResult");
  result->tag = TAG_SNAP_COMMIT;
  result->success = success != 0;
  return reg_local(result);
}

// Decode one UTF-8 code point. Invalid input is consumed one byte at a time and
// represented by U+FFFD, matching the non-throwing behaviour Unity expects from
// Java strings.
static uint32_t utf8_next(const unsigned char **cursor) {
  const unsigned char *p = *cursor;
  uint32_t cp;
  int count;

  if (p[0] < 0x80) {
    *cursor = p + 1;
    return p[0];
  }
  if ((p[0] & 0xe0) == 0xc0) {
    cp = p[0] & 0x1f;
    count = 2;
  } else if ((p[0] & 0xf0) == 0xe0) {
    cp = p[0] & 0x0f;
    count = 3;
  } else if ((p[0] & 0xf8) == 0xf0) {
    cp = p[0] & 0x07;
    count = 4;
  } else {
    *cursor = p + 1;
    return 0xfffd;
  }

  for (int i = 1; i < count; i++) {
    if (!p[i] || (p[i] & 0xc0) != 0x80) {
      *cursor = p + 1;
      return 0xfffd;
    }
    cp = (cp << 6) | (p[i] & 0x3f);
  }
  *cursor = p + count;

  const uint32_t minimum = count == 2 ? 0x80u : (count == 3 ? 0x800u : 0x10000u);
  if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return 0xfffd;
  return cp;
}

// Convert UTF-8 to Java UTF-16. When out is NULL this only counts code units.
static juint fake_utf8_to_utf16(const char *str, uint16_t *out) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    uint32_t cp = utf8_next(&p);
    if (cp < 0x10000) {
      if (out) out[n] = (uint16_t)cp;
      n++;
    } else {
      cp -= 0x10000;
      if (out) {
        out[n] = (uint16_t)(0xd800u + (cp >> 10));
        out[n + 1] = (uint16_t)(0xdc00u + (cp & 0x3ff));
      }
      n += 2;
    }
  }
  return n;
}

// UTF-16 code-unit count of a UTF-8 string (Java String.length()).
static juint utf16_len(const char *str) {
  return fake_utf8_to_utf16(str, NULL);
}

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

#define MAX_CLASSES 1024
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) {
    
    return &class_pool[0];
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// The engine fetches the ClassLoader every frame; hand back a cached singleton
// (pooled, never reg_local'd) so it doesn't fill the local-ref table.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 8192
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

static const char *first_string_arg(const char *sig, va_list va);

static const char *lang_code(void) {
  if (config.language == LANG_JA) return "ja";
  if (config.language == LANG_EN) return "en";
  // LANG_AUTO: resolve the Switch system language once
  static const char *cached = NULL;
  if (!cached) {
    cached = "en";
    u64 code; SetLanguage sl;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl))) {
        switch (sl) {
          case SetLanguage_JA:                            cached = "ja"; break;
          case SetLanguage_FR: case SetLanguage_FRCA:     cached = "fr"; break;
          case SetLanguage_DE:                            cached = "de"; break;
          case SetLanguage_IT:                            cached = "it"; break;
          case SetLanguage_ES: case SetLanguage_ES419:    cached = "es"; break;
          case SetLanguage_PT: case SetLanguage_PTBR:     cached = "pt"; break;
          case SetLanguage_NL:                            cached = "nl"; break;
          case SetLanguage_RU:                            cached = "ru"; break;
          case SetLanguage_KO:                            cached = "ko"; break;
          case SetLanguage_ZHCN: case SetLanguage_ZHHANS:
          case SetLanguage_ZHTW: case SetLanguage_ZHHANT: cached = "zh"; break;
          default:                                        cached = "en"; break;
        }
      }
      setExit();
    }
  }
  return cached;
}

/* Locale getters consistent with lang_code() (libunity builds its culture string
 * as getLanguage()+"-"+getCountry()). */
typedef struct { const char *lang, *ctry, *iso3l, *iso3c, *loc; } LangRow;
static const LangRow *lang_row(void) {
  static const LangRow rows[] = {
    {"en","US","eng","USA","en_US"}, {"ja","JP","jpn","JPN","ja_JP"},
    {"fr","FR","fra","FRA","fr_FR"}, {"de","DE","deu","DEU","de_DE"},
    {"it","IT","ita","ITA","it_IT"}, {"es","ES","spa","ESP","es_ES"},
    {"pt","PT","por","PRT","pt_PT"}, {"nl","NL","nld","NLD","nl_NL"},
    {"ru","RU","rus","RUS","ru_RU"}, {"ko","KR","kor","KOR","ko_KR"},
    {"zh","CN","zho","CHN","zh_CN"},
  };
  const char *l = lang_code();
  for (unsigned i = 0; i < sizeof rows / sizeof *rows; i++)
    if (!strcmp(rows[i].lang, l)) return &rows[i];
  return &rows[0];
}

const char *jni_locale_name(void) {
  return lang_row()->loc;
}

/* AndroidJavaObject wraps a JNI jobject by asking GetObjectClass(). Our opaque
 * pooled objects normally report java/lang/Object, which is fine for generic
 * placeholders but loses java.util.Locale's identity. Rovio's Locale helper then
 * resolves instance methods (getCountry/toLanguageTag) on Object. Recognize only
 * our interned Locale handle here so other intentionally-generic objects keep the
 * old behaviour. */
static int is_pooled_object_named(const void *obj, const char *label) {
  const uintptr_t p = (uintptr_t)obj;
  const uintptr_t first = (uintptr_t)&iobj_pool[0];
  const uintptr_t last = (uintptr_t)&iobj_pool[MAX_IOBJ];
  if (!obj || p < first || p >= last)
    return 0;
  const FakeObject *o = obj;
  return o->tag == TAG_CLASS && !strcmp(o->label, label);
}

static int is_locale_object(const void *obj) {
  return is_pooled_object_named(obj, "java/util/Locale");
}

static void *locale_method_result(const char *method) {
  const LangRow *lr = lang_row();
  const char *value = NULL;
  if (!strcmp(method, "getLanguage"))          value = lr->lang;
  else if (!strcmp(method, "getCountry"))      value = lr->ctry;
  else if (!strcmp(method, "getISO3Language")) value = lr->iso3l;
  else if (!strcmp(method, "getISO3Country"))  value = lr->iso3c;
  else if (!strcmp(method, "toString") || name_has(method, "getDisplayName"))
    value = lr->loc;
  if (!strcmp(method, "toLanguageTag")) {
    static char tag[16];
    snprintf(tag, sizeof tag, "%s-%s", lr->lang, lr->ctry);
    value = tag;
  }
  if (value) {
    return jni_make_string(value);
  }
  return NULL;
}

// Return the first non-empty String argument in a JNI argument list.
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

/* jni_string_utf is defined far below (shared with unity_jni.c) and unity_jni.h
 * is included after this point, so forward-declare it for act_object's
 * getProperty()/locale arg reads. */
const char *jni_string_utf(void *jstr);

/* Return Switch audio parameters for FMOD's Android AudioManager query. */
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  if (which == 1) return jni_make_string("48000");   /* native output sample rate */
  if (which == 2) return jni_make_string("256");     /* native frames-per-buffer */
  return jni_make_string("");
}

// Unity's AndroidJavaObject overload resolver calls the Java-side
// ReflectionHelper first, then passes the returned Method/Field to
// FromReflectedMethod/FromReflectedField. Keep the requested target identity in
// our pooled FakeID instead of replacing every reflected call with invoke()V.
static void *reflection_object_v(const FakeID *id, va_list va, int *handled) {
  *handled = 0;
  if (!name_has(id->cls, "unity3d/player/ReflectionHelper"))
    return NULL;

  if (!strcmp(id->name, "getConstructorID")) {
    void *target = va_arg(va, void *);
    const char *sig = obj_str(va_arg(va, void *));
    *handled = 1;
    return get_id(class_name_of(target), "<init>", sig);
  }
  if (!strcmp(id->name, "getMethodID") || !strcmp(id->name, "getFieldID")) {
    void *target = va_arg(va, void *);
    const char *name = obj_str(va_arg(va, void *));
    const char *sig = obj_str(va_arg(va, void *));
    (void)va_arg(va, int); // isStatic
    *handled = 1;
    return get_id(class_name_of(target), name, sig);
  }
  if (!strcmp(id->name, "getFieldSignature")) {
    FakeID *field = va_arg(va, void *);
    *handled = 1;
    return jni_make_string(field && field->tag == TAG_ID ? field->sig : "Ljava/lang/Object;");
  }
  if (!strcmp(id->name, "newProxyInstance") ||
      !strcmp(id->name, "createInvocationError")) {
    *handled = 1;
    return jni_make_object("com/unity3d/player/ReflectionHelper$Proxy");
  }
  return NULL;
}

static void *reflection_object_a(const FakeID *id, const void *args, int *handled) {
  *handled = 0;
  if (!name_has(id->cls, "unity3d/player/ReflectionHelper") || !args)
    return NULL;

  void *const *a = (void *const *)args;
  if (!strcmp(id->name, "getConstructorID")) {
    *handled = 1;
    return get_id(class_name_of(a[0]), "<init>", obj_str(a[1]));
  }
  if (!strcmp(id->name, "getMethodID") || !strcmp(id->name, "getFieldID")) {
    *handled = 1;
    return get_id(class_name_of(a[0]), obj_str(a[1]), obj_str(a[2]));
  }
  if (!strcmp(id->name, "getFieldSignature")) {
    FakeID *field = a[0];
    *handled = 1;
    return jni_make_string(field && field->tag == TAG_ID ? field->sig : "Ljava/lang/Object;");
  }
  if (!strcmp(id->name, "newProxyInstance") ||
      !strcmp(id->name, "createInvocationError")) {
    *handled = 1;
    return jni_make_object("com/unity3d/player/ReflectionHelper$Proxy");
  }
  return NULL;
}

static void *act_object(const FakeID *id, va_list va) {
  int reflection_handled = 0;
  void *reflection_result = reflection_object_v(id, va, &reflection_handled);
  if (reflection_handled)
    return reflection_result;

  if (!strcmp(id->cls, "com/halfbrick/mortar/KeyStore") &&
      !strcmp(id->name, "GetValue") &&
      !strcmp(id->sig,
              "(Ljava/lang/String;)Ljava/lang/String;")) {
    const char *key = obj_str(va_arg(va, void *));
    char *value = local_keystore_get_copy(key);
    void *result = value ? jni_make_string(value) : NULL;
    free(value);
    return result;
  }

  /* Firebase requires a non-null platform-info registrar. */
  if (name_has(id->cls, "firebase/platforminfo/GlobalLibraryVersionRegistrar")) {
    if (!strcmp(id->name, "getInstance"))
      return jni_make_object("com/google/firebase/platforminfo/GlobalLibraryVersionRegistrar");
    if (!strcmp(id->name, "getRegisteredVersions"))
      return jni_make_object("java/util/Set");
  }

  /* Report the Android Choreographer as unavailable (null) so the engine takes its
   * non-vsync fallback instead of blocking on a vsync callback we can't deliver.
   * Covers Swappy's SwappyDisplayManager too. Must precede the generic handlers. */
  if ((name_has(id->cls, "Choreographer") || name_has(id->cls, "SwappyDisplayManager")) &&
      (name_has(id->name, "getInstance") || sig_returns(id->sig, "Landroid/view/Choreographer;")))
    return NULL;
  /* Uri.encode/decode: Unity round-trips PlayerPrefs keys through these. We are
   * the storage, so identity (return the input string) round-trips correctly and
   * keeps keys non-empty. Must precede the generic handlers. */
  if ((name_has(id->cls, "net/Uri") || sig_returns(id->sig,"Ljava/lang/String;")) &&
      (!strcmp(id->name, "encode") || !strcmp(id->name, "decode"))) {
    void *arg = va_arg(va, void *);
    /* Unity 6's collapsed Object static wrapper supplies its cached jclass as
     * the first variadic item. It cannot be a String; the next item is the key. */
    if (arg && *(uint32_t *)arg == TAG_CLASS) arg = va_arg(va, void *);
    return arg;
  }
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  /* Nice Vibrations stores Context.getSystemService(VIBRATOR_SERVICE) during its
   * static initializer. The stripped AndroidJavaObject call returns Object, so a
   * null generic fallback poisons the type initializer and stalls scene startup.
   * Keep a valid no-op vibrator handle; hasVibrator() remains false in act_int. */
  if (!strcmp(id->name, "getSystemService"))
    return jni_make_object("android/os/Vibrator");
  /* Unity Mobile Notifications obtains its Java singleton through an
   * AndroidJavaClass.CallStatic<AndroidJavaObject>. In this stripped build the
   * generated JNI signature returns Object, so the generic object fallback is
   * otherwise NULL even though initialization requires a live manager handle. */
  if (!strcmp(id->name, "getNotificationManagerImpl"))
    return jni_make_object("com/unity/androidnotifications/UnityNotificationManager");
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string(GAME_VERSION_NAME);
  if (name_has(id->name, "PackageName")) return jni_make_string(GAME_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  // Environment.getExternalStorageState() must return the SAME token as the
  // Environment.MEDIA_MOUNTED field ("mounted", see field_object) or the engine
  // decides external storage is unavailable and the save path never initialises.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // Locale.getDefault() is declared through AndroidJavaObject's generic bridge as
  // returning Object in this Unity build, not Locale. Match the class+method rather
  // than relying only on the JNI return signature, or it falls through to NULL and
  // Rovio.Abba.Frameworks.Localization.Locale.GetCurrentLocale NREs.
  if (name_has(id->cls, "Locale")) {
    if (!strcmp(id->name, "getDefault")) {
      return jni_make_object("java/util/Locale");
    }
    void *result = locale_method_result(id->name);
    if (result) return result;
  }
  if (!strcmp(id->name, "getPackageCodePath") ||
      !strcmp(id->name, "getPackageResourcePath"))
    return jni_make_string(managed_path(GAME_HOME "/assets.apk"));
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(GAME_HOME));
  // Beacon parses getPlatformIdentifiers() immediately and assumes a JSON
  // object. A null/empty generic Android return throws DeviceInfo's static
  // initializer and prevents the first scene from ever completing.
  if (!strcmp(id->name, "getPlatformIdentifiers"))
    return jni_make_string("{\"installSource\":\"\",\"androidId\":\"\",\"buildId\":\"Switch\",\"isChromeOS\":false}");
  if (!strcmp(id->name, "getMetaInstallReferrer"))
    return jni_make_string("");
  if (name_has(id->cls, "beacon/Localization"))
    return jni_make_string(lang_row()->loc);
  if (name_has(id->cls, "beacon/DeviceInfo")) {
    if (name_has(id->name, "Locale") || name_has(id->name, "locale"))
      return jni_make_string(lang_row()->loc);
    if (sig_returns(id->sig, "Ljava/lang/String;"))
      return jni_make_string("");
  }
  // Android object getters that must NOT be null (getPackageInfo/getApplicationInfo
  // reach PackageInfo.versionName/versionCode -> Application.version); a null here
  // blanked the version. Hand back live opaque objects for field_object to service.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  // Locale.getDefault() must be non-null or the engine's culture resolves to
  // SystemLanguage.Unknown and the game is forced to English.
  if (sig_returns(id->sig, "Ljava/util/Locale;"))
    return jni_make_object("java/util/Locale");
  /* AndroidJavaObject.Call<byte[]>() must receive a real JNI primitive array.
   * Returning NULL here becomes a managed null byte[]; the treasure/reward flow
   * then faults in File.WriteAllBytes and never releases its interaction layer.
   * An unsupported Android serializer has no payload on Switch, so use a valid
   * empty array: File.WriteAllBytes completes and the owning async task follows
   * its normal modal-cleanup path. */
  if (sig_returns(id->sig, "[B")) {
    return make_pri_array_adopt(malloc(1), 0, 1);
  }
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string(""); // UUID, asset-pack name, etc.
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  if (!strcmp(id->cls, "com/halfbrick/mortar/KeyStore")) {
    if (!strcmp(id->name, "SetValue") &&
        !strcmp(id->sig,
                "(Ljava/lang/String;Ljava/lang/String;)Z")) {
      const char *key = obj_str(va_arg(va, void *));
      const char *value = obj_str(va_arg(va, void *));
      return (juint)local_keystore_set_value(key, value);
    }
    if (!strcmp(id->name, "SetValueIf") &&
        !strcmp(id->sig,
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z")) {
      const char *key = obj_str(va_arg(va, void *));
      const char *value = obj_str(va_arg(va, void *));
      const char *expected = obj_str(va_arg(va, void *));
      return (juint)local_keystore_set_value_if(key, value, expected);
    }
  }

  /* Audio is provided by the SDL-backed OpenSL implementation. */
  if (name_has(id->cls, "halfbrick/mortar/NativeGameLib") &&
      !strcmp(id->name, "SupportsOpenSL") &&
      !strcmp(id->sig, "()Z"))
    return 1;

  if (name_has(id->cls, "halfbrick/mortar/GooglePlayPurchaseV2Service") &&
      !strcmp(id->name, "Initialise") &&
      !strcmp(id->sig, "()Z"))
    return 1;
  // Integer.parseInt / Long.parseLong: FMOD parses getProperty()'s results through
  // these; the old 0 fall-through made framesPerBuffer 0 -> FMOD error 60.
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    
    return v;
  }
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  /* Queue callbacks after their request methods return. */
  if (name_has(id->cls, "halfbrick/mortar/GooglePlaySnapshotManager") &&
      !strcmp(id->name, "Open") &&
      !strcmp(id->sig,
              "(Landroid/content/Context;Ljava/lang/String;Z)I")) {
    (void)va_arg(va, void *); /* Context */
    (void)va_arg(va, void *); /* Name */
    (void)va_arg(va, int);    /* Create if missing */
    int request_id = 0;
    mutexLock(&g_snapshot_callbacks.lock);
    if (g_snapshot_callbacks.open_count < MAX_DEFERRED_SNAPSHOT_OPENS) {
      request_id = g_snapshot_callbacks.next_request_id++;
      if (g_snapshot_callbacks.next_request_id <= 0)
        g_snapshot_callbacks.next_request_id = 1;
      const unsigned tail =
          (g_snapshot_callbacks.open_head + g_snapshot_callbacks.open_count) %
          MAX_DEFERRED_SNAPSHOT_OPENS;
      g_snapshot_callbacks.open_request_ids[tail] = request_id;
      ++g_snapshot_callbacks.open_count;
    }
    mutexUnlock(&g_snapshot_callbacks.lock);
    return (juint)request_id;
  }
  if (name_has(id->cls, "halfbrick/mortar/GooglePlaySnapshotManager") &&
      !strcmp(id->name, "CommitSnapshot") &&
      !strcmp(id->sig,
              "(Landroid/content/Context;Ljava/lang/String;"
              "Lcom/google/android/gms/games/snapshot/Snapshot;"
              "Lcom/halfbrick/mortar/GooglePlaySnapshotManager$MetadataChange;)I")) {
    (void)va_arg(va, void *); /* Context */
    (void)va_arg(va, void *); /* Name */
    (void)va_arg(va, void *); /* Snapshot */
    (void)va_arg(va, void *); /* MetadataChange */
    int request_id = 0;
    mutexLock(&g_snapshot_callbacks.lock);
    if (g_snapshot_callbacks.commit_count < MAX_DEFERRED_SNAPSHOT_COMMITS) {
      request_id = g_snapshot_callbacks.next_request_id++;
      if (g_snapshot_callbacks.next_request_id <= 0)
        g_snapshot_callbacks.next_request_id = 1;
      const unsigned tail =
          (g_snapshot_callbacks.commit_head +
           g_snapshot_callbacks.commit_count) % MAX_DEFERRED_SNAPSHOT_COMMITS;
      g_snapshot_callbacks.commit_request_ids[tail] = request_id;
      g_snapshot_callbacks.commit_success[tail] =
          (uint8_t)(g_snapshot_callbacks.last_write_ok != 0);
      g_snapshot_callbacks.last_write_ok = 1;
      ++g_snapshot_callbacks.commit_count;
    }
    mutexUnlock(&g_snapshot_callbacks.lock);
    return (juint)request_id;
  }
  // isGooglePlayServicesAvailable(): return SERVICE_MISSING (1) so the Play Games
  // plugin cleanly disables itself instead of signing in against absent GMS.
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  // Android permissions are provided by the host environment.
  if (name_has(id->name, "checkPermission")     || name_has(id->name, "hasPermission") ||
      name_has(id->name, "isPermissionGranted") || name_has(id->name, "checkNotificationStatus"))
    return 1;
  if (name_has(id->name, "shouldShowRequestPermissionRationale")) return 0;
  if (name_has(id->cls, "ConsentManagementPlatform")) {
    if (!strcmp(id->name, "Done"))
      return 1;
    if (!strcmp(id->name, "IsConsentGiven") ||
        !strcmp(id->name, "ArePrivacyOptionsRequired"))
      return 0;
  }
  (void)va;
  // Unsupported boolean queries default to false.
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  if (name_has(id->cls, "halfbrick/mortar/GooglePlaySnapshotManager") &&
      !strcmp(id->name, "WriteSnapshot") &&
      !strcmp(id->sig,
              "(Lcom/google/android/gms/games/snapshot/Snapshot;[B)V")) {
    (void)va_arg(va, void *); /* opaque local Snapshot handle */
    FakePriArray *bytes = va_arg(va, FakePriArray *);
    const int valid = bytes && bytes->tag == TAG_PRIARR &&
                      bytes->elem_size == 1 && bytes->len >= 0;
    const int write_ok = valid &&
                         local_snapshot_write(bytes->data, bytes->len);
    mutexLock(&g_snapshot_callbacks.lock);
    g_snapshot_callbacks.last_write_ok = write_ok;
    mutexUnlock(&g_snapshot_callbacks.lock);
    return;
  }

  if (name_has(id->cls, "halfbrick/mortar/GooglePlayPurchaseV2Service")) {
    if (!strcmp(id->name, "RequestProductsInfo") &&
        !strcmp(id->sig, "(Landroid/os/Bundle;J)V")) {
      FakeBundle *bundle = va_arg(va, FakeBundle *);
      const int64_t request_id = va_arg(va, int64_t);
      unsigned count = 0;

      mutexLock(&g_store_callbacks.lock);
      if (g_store_callbacks.info_count < MAX_PRODUCT_INFO_REQUESTS) {
        const unsigned tail =
            (g_store_callbacks.info_head + g_store_callbacks.info_count) %
            MAX_PRODUCT_INFO_REQUESTS;
        ProductInfoRequest *request = &g_store_callbacks.info[tail];
        memset(request, 0, sizeof(*request));
        request->request_id = request_id;
        if (bundle && bundle->tag == TAG_BUNDLE) {
          count = bundle->count;
          if (count > MAX_BUNDLE_KEYS) count = MAX_BUNDLE_KEYS;
          request->product_count = count;
          for (unsigned i = 0; i < count; ++i)
            snprintf(request->product_ids[i], sizeof request->product_ids[i],
                     "%s", bundle->keys[i]);
        }
        ++g_store_callbacks.info_count;
      }
      mutexUnlock(&g_store_callbacks.lock);
      return;
    }

    if (!strcmp(id->name, "RequestPurchase") &&
        !strcmp(id->sig, "(Ljava/lang/String;)V")) {
      const char *product_id = obj_str(va_arg(va, void *));
      mutexLock(&g_store_callbacks.lock);
      if (g_store_callbacks.purchase_count < MAX_DEFERRED_PURCHASES) {
        const unsigned tail =
            (g_store_callbacks.purchase_head +
             g_store_callbacks.purchase_count) % MAX_DEFERRED_PURCHASES;
        snprintf(g_store_callbacks.purchase_ids[tail],
                 sizeof g_store_callbacks.purchase_ids[tail], "%s",
                 product_id ? product_id : "");
        ++g_store_callbacks.purchase_count;
      }
      mutexUnlock(&g_store_callbacks.lock);
      return;
    }

    if (!strcmp(id->name, "RequestPendingPurchases")) {
      mutexLock(&g_store_callbacks.lock);
      ++g_store_callbacks.pending_end_count;
      mutexUnlock(&g_store_callbacks.lock);
      return;
    }

    if (!strcmp(id->name, "RestorePurchases")) {
      mutexLock(&g_store_callbacks.lock);
      ++g_store_callbacks.restore_complete_count;
      mutexUnlock(&g_store_callbacks.lock);
      return;
    }

    if (!strcmp(id->name, "ConsumePurchase") ||
        !strcmp(id->name, "AcknowledgePurchase")) {
      (void)va_arg(va, void *);
      return;
    }
  }

  if (name_has(id->cls, "firebase/platforminfo/GlobalLibraryVersionRegistrar") &&
      !strcmp(id->name, "registerVersion")) {
    return;
  }
  if (name_has(id->cls, "ConsentManagementPlatform") &&
      !strcmp(id->name, "EnableFormLoad")) {
    return;
  }
  if (!strcmp(id->name, "showSoftInput")) {
    const char *initial = obj_str(va_arg(va, void *));
    const int keyboard_type = va_arg(va, int);
    (void)va_arg(va, int); /* autocorrection */
    (void)va_arg(va, int); /* multiline */
    const int secure = va_arg(va, int);
    (void)va_arg(va, int); /* alert */
    const char *guide = obj_str(va_arg(va, void *));
    int character_limit = va_arg(va, int);
    (void)va_arg(va, int); /* hide input field */
    if (character_limit <= 0 || character_limit > 500)
      character_limit = 500;

    mutexLock(&g_soft_keyboard.lock);
    snprintf(g_soft_keyboard.initial, sizeof g_soft_keyboard.initial,
             "%s", initial ? initial : "");
    snprintf(g_soft_keyboard.guide, sizeof g_soft_keyboard.guide,
             "%s", guide ? guide : "");
    g_soft_keyboard.keyboard_type = keyboard_type;
    g_soft_keyboard.secure = secure != 0;
    g_soft_keyboard.character_limit = character_limit;
    g_soft_keyboard.pending = 1;
    mutexUnlock(&g_soft_keyboard.lock);
    return;
  }
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

/* Delegate Unity and input classes to their stateful handlers. */
#include "unity_input.h"

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  /* Android's notification package asks currentActivity.getClass() and wraps the
   * returned java.lang.Class in an AndroidJavaObject before creating its manager.
   * Unity resolves this call as Object.getClass():Object, so the signature alone
   * cannot trigger act_object's Class fallback. Preserve the receiver's pooled
   * type here; returning NULL aborts GameNotificationsManager initialization. */
  if (recv && !strcmp(id->name, "getClass")) {
    const char *name = class_name_of(recv);
    return intern_class(name && name[0] ? name : "java/lang/Object");
  }
  /* Preserve native input events copied through MotionEvent.obtain(). */
  if (!strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes([charset]) -> the string's UTF-8 bytes (Unity's PlayerPrefs key
   * encoding needs real bytes or every encoded key collides). Route by receiver. */
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  /* Stateful/pooled receivers are routed below to their specialised module.
   * None of those modules implements a byte[] result, and returning its generic
   * NULL used to bypass act_object's non-null fallback. Enforce the JNI contract
   * before receiver ownership can swallow an unsupported Android serializer. */
  if (sig_returns(id->sig, "[B")) {
    return make_pri_array_adopt(malloc(1), 0, 1);
  }
  /* The wrapped Locale instance's method IDs may be resolved against
   * java/lang/Object (see is_locale_object). Route by receiver identity so
   * getLanguage/getCountry/toLanguageTag still return real Switch locale data. */
  if (is_locale_object(recv)) {
    void *result = locale_method_result(id->name);
    if (result) return result;
  }
  // Reflected Method/Field handles retain their declaring class in FakeID.
  if (recv && *(uint32_t *)recv == TAG_ID && !strcmp(id->name, "getDeclaringClass"))
    return intern_class(((FakeID *)recv)->cls);
  if (unity_owns_recv(recv) || !strcmp(id->name,"getSharedPreferences") ||
      unity_owns_class(id->cls))
    return unity_dispatch_object(recv, id, va);
  return act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  // String instance methods via CallIntMethod (length() sizes path buffers). The
  // receiver is our FakeString reported as java/lang/Object, so route on the receiver
  // tag + method name, not id->cls (else length() returns 0 and buffers overflow).
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
  }
  /* Boxed PlayerPrefs value (Integer/Long/Boolean) from getAll(): unbox by the
   * receiver so only our own boxes are affected. intValue/longValue/booleanValue
   * all land here (CallInt/Long/BooleanMethod -> dispatch_int). */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* MotionEvent/KeyEvent getters: the engine resolves these via
   * GetObjectClass(event) -> java/lang/Object, so id->cls is NOT the real
   * class. Route on the receiver tag (mirrors the FakeString case above), or
   * touch getters silently fall through to act_int and return 0. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) return unity_dispatch_float(recv,id,va);
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  /* JNIWrapper::Bundle uses putString(productId, productId) to marshal the set
   * requested from Google Play.  Keep those keys on the individual Bundle so
   * the later RequestProductsInfo call can return metadata for the exact live
   * configuration, including future APK updates. */
  if (recv && *(uint32_t *)recv == TAG_BUNDLE &&
      !strcmp(id->name, "putString")) {
    FakeBundle *bundle = recv;
    const char *key = obj_str(va_arg(va, void *));
    (void)va_arg(va, void *); /* value: the native bridge repeats the id */
    if (key && key[0]) {
      unsigned i;
      for (i = 0; i < bundle->count; ++i)
        if (!strcmp(bundle->keys[i], key)) break;
      if (i == bundle->count && bundle->count < MAX_BUNDLE_KEYS) {
        snprintf(bundle->keys[bundle->count],
                 sizeof bundle->keys[bundle->count], "%s", key);
        ++bundle->count;
      }
    }
    return;
  }

  /* Dispatch Mortar worker threads through their registered JNI entry point. */
  if (g_mortar_native_thread_entry &&
      name_has(id->cls, "halfbrick/mortar/NativeGameLib") &&
      !strcmp(id->name, "native_threadEntry") &&
      !strcmp(id->sig, "(J)V")) {
    const int64_t native_thread = va_arg(va, int64_t);
    g_mortar_native_thread_entry(fake_env, recv, native_thread);
    return;
  }
  if (unity_owns_recv(recv) || unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (obj && *(uint32_t *)obj == TAG_BUNDLE)
    return intern_class("android/os/Bundle");
  if (obj && *(uint32_t *)obj == TAG_SNAP_OPEN)
    return intern_class(
        "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotOpenResult");
  if (obj && *(uint32_t *)obj == TAG_SNAP_COMMIT)
    return intern_class(
        "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotCommitResult");
  /* Unity 6 asks GetObjectClass(InputEvent) first and resolves either the KeyEvent
   * or MotionEvent getter set from that result.  Reporting java/lang/Object made a
   * real touch resolve getKeyCode(), so nativeInjectEvent rejected it before it
   * ever reached Unity's touch queue.  Preserve the exact kind carried by UEvent;
   * copies returned by MotionEvent.obtain retain the same tag/kind. */
  if (input_owns_recv(obj))
    return intern_class(input_recv_is_motion(obj)
                        ? "android/view/MotionEvent"
                        : "android/view/KeyEvent");
  /* Keep the single typed Android object required by managed boot code typed.
   * AndroidJavaObject(IntPtr) caches this class and uses it to resolve the next
   * instance method. Reporting Object here makes Locale.getDefault() succeed,
   * but its following toLanguageTag()/getCountry() call resolve on Object and
   * return null to Locale.GetCurrentLocale(). */
  if (is_locale_object(obj)) return intern_class("java/util/Locale");
  if (is_pooled_object_named(
          obj,
          "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotOpenResult"))
    return intern_class(
        "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotOpenResult");
  if (is_pooled_object_named(
          obj,
          "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotCommitResult"))
    return intern_class(
        "com/halfbrick/mortar/GooglePlaySnapshotManager$SnapshotCommitResult");
  const char *uclass=unity_recv_class(obj);
  if (uclass) return intern_class(uclass);
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* String(byte[][,charset]) constructor: decode the byte array (UTF-8) into a real
 * FakeString so Unity's PlayerPrefs keys aren't all empty and colliding. Other
 * ctors are unaffected (still a labelled object). */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  FakeID *m = mid;
  if (cn && !strcmp(cn, "android/os/Bundle")) {
    FakeBundle *bundle = calloc(1, sizeof(*bundle));
    if (!bundle) return NULL;
    bundle->tag = TAG_BUNDLE;
    return reg_local(bundle);
  }
  /* Unity 6 sometimes resolves this constructor against java/lang/Object even
   * though the exact signature is Java String(byte[], charset). Recognise the
   * unambiguous constructor signature as well as the nominal class; otherwise
   * the result becomes a generic pooled object and its later getBytes() call can
   * produce managed null (also collapsing every encoded PlayerPrefs key). */
  const int byte_string_ctor = m && !strcmp(m->name, "<init>") &&
                               !strcmp(m->sig, "([BLjava/lang/String;)V");
  if ((cn && strstr(cn, "java/lang/String")) || byte_string_ctor) {
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid); void *a0 = va_arg(va, void *); va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; void *a0 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf (slot 32). We can't track the runtime type of opaque fake jobjects,
 * so answer optimistically (assume the cast succeeds) -- returning 0 trapped the game
 * in a per-frame retry loop. Exact answers for input events and boxed values below. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* nativeInjectEvent classifies the event by instanceof KeyEvent/MotionEvent, so
   * answer by the handle's real kind (a blind 1 gets a touch read as a key). */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    if (strstr(cn, "InputEvent"))  return 1;
    /* On this Unity 6 build the nativeInjectEvent caches sometimes contain an
     * untyped java/lang/Object handle.  Optimistically returning true made its
     * first (KeyEvent) test win for every touch.  Use the verified two-test
     * order only for an untyped handle; exact class names above remain exact. */
    const juint answer=(juint)input_instanceof_untyped(obj);
    return answer;
  }
  /* Boxed PlayerPrefs values from getAll(): Unity reads each value with
   * IsInstanceOf(value, Integer/Long/Float/Boolean/String) then unboxes. These
   * MUST be exact or every value is misread as the first type checked. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
    /* other classes: fall through to the optimistic answer below */
  }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }
static juint j_MonitorNoop(void *env, void *object) {
  (void)env;
  (void)object;
  return 0;
}

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
// Route jvalue[] calls through the same name-based compatibility dispatch.
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  int reflection_handled = 0;
  void *reflection_result = reflection_object_a(id, a, &reflection_handled);
  if (reflection_handled)
    return reflection_result;
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && (name_has(id->cls, "net/Uri") || sig_returns(id->sig,"Ljava/lang/String;")) &&
      (!strcmp(id->name, "encode") || !strcmp(id->name, "decode"))) {
    void *arg = (void *)((void *const *)a)[0];
    if (arg && *(uint32_t *)arg == TAG_CLASS) arg = (void *)((void *const *)a)[1];
    return arg;                                  /* identity, jvalue path */
  }
  void *out=NULL;
  if (unity_dispatch_object_a(r,id,a,&out)) return out;
  return j_CallObjectMethod(e, r, id);
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallBooleanMethod(e,r,id); }
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    
    return v;
  }
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallIntMethod(e, r, id);
}
static juint j_CallLongMethodA   (void *e, void *r, FakeID *id, const void *a){
  uint64_t out=0; if(unity_dispatch_int_a(r,id,a,&out))return (juint)out;
  return j_CallLongMethod(e,r,id); }
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){
  float out=0; if(unity_dispatch_float_a(r,id,a,&out))return out;
  return j_CallFloatMethod(e,r,id); }
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){
  if(unity_dispatch_void_a(r,id,a))return;
  j_CallVoidMethod(e,r,id); }
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t cp = u[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < len &&
        u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
      cp = 0x10000u + ((cp - 0xd800u) << 10) + (u[++i] - 0xdc00u);
    } else if (cp >= 0xd800 && cp <= 0xdfff) {
      cp = 0xfffd;
    }
    if (cp < 0x80) {
      tmp[o++] = (char)cp;
    } else if (cp < 0x800) {
      tmp[o++] = (char)(0xc0 | (cp >> 6));
      tmp[o++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      tmp[o++] = (char)(0xe0 | (cp >> 12));
      tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      tmp[o++] = (char)(0x80 | (cp & 0x3f));
    } else {
      tmp[o++] = (char)(0xf0 | (cp >> 18));
      tmp[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
      tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      tmp[o++] = (char)(0x80 | (cp & 0x3f));
    }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// JNI slots 165/166. Unity uses these while initializing Android audio and
// localization. The returned storage belongs to the caller until ReleaseStringChars.
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *utf = obj_str(jstr);
  const juint len = utf16_len(utf);
  uint16_t *chars = malloc(((size_t)len + 1) * sizeof(*chars));
  if (!chars) {
    if (is_copy) *is_copy = 0;
    return NULL;
  }
  fake_utf8_to_utf16(utf, chars);
  chars[len] = 0;
  if (is_copy) *is_copy = 1;
  return chars;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env;
  (void)jstr;
  free((void *)chars);
}

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 offsets and output, including surrogate pairs.
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)utf16_len(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  uint16_t *all = malloc(((size_t)slen + 1) * sizeof(*all));
  if (!all) return;
  fake_utf8_to_utf16(s, all);
  memcpy(buf, all + start, (size_t)len * sizeof(*buf));
  free(all);
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewShortArray(void *env, int len) { (void)env; return new_pri_array(len, 2); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and game read Java fields (android.os.Build.*, Build.VERSION.SDK_INT,
// PackageInfo.versionName/versionCode, DisplayMetrics.*). Route every read through a
// name-based dispatcher on the FakeID; a universal null/0 blanked the app version and
// zeroed display metrics.
#define APP_VERSION_NAME GAME_VERSION_NAME
#define APP_VERSION_CODE GAME_VERSION_CODE
#define NX_SDK_INT       33        /* Android 13 -- high enough to pass any minSdk gate  */

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  // UnityPlayer.currentActivity is THE Activity -- null NPEs every currentActivity
  // .getXxx() in managed code, so hand back a live opaque Activity.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    // Some plugins use UnityPlayer.activityContext instead of currentActivity.
    if (!strcmp(n, "activityContext"))  return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
    // Keep the Android permissions plugin handle non-null.
    if (!strcmp(n, "androidPermissionsPlugin")) {
      void *plugin = jni_make_object("com/rovio/permissions/AndroidPermissionsPlugin");
      
      return plugin;
    }
  }
  // AudioManager.PROPERTY_OUTPUT_* static keys, read just before getProperty(key).
  // Record which one in g_last_output_prop so getProperty() can answer when the key
  // argument is lost on the JNI call path (see getproperty_value).
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (name_has(c, "GooglePlaySnapshotManager$SnapshotOpenResult") &&
      !strcmp(n, "isSuccess"))
    return 1;
  if (name_has(c, "GooglePlaySnapshotManager$SnapshotCommitResult") &&
      !strcmp(n, "isSuccess"))
    return 1;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return 720;
    if (!strcmp(n, "heightPixels"))return 1280;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return 720;
    if (!strcmp(n, "heightPixels")) return 1280;
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env;
  if (!fid) return NULL;
  const FakeID *id = fid;
  if (obj && *(uint32_t *)obj == TAG_SNAP_OPEN) {
    FakeSnapshotOpenResult *result = obj;
    if (!strcmp(id->name, "data")) {
      void *copy = malloc(result->data_len > 0
                              ? (size_t)result->data_len
                              : 1);
      if (!copy) return NULL;
      if (result->data_len > 0)
        memcpy(copy, result->data, (size_t)result->data_len);
      return make_pri_array_adopt(copy, result->data_len, 1);
    }
    if (!strcmp(id->name, "snapshot")) return result->snapshot;
    return NULL; /* no conflict or conflicting payloads in the local backend */
  }
  if (obj && *(uint32_t *)obj == TAG_SNAP_COMMIT)
    return NULL; /* successful local commits never carry a conflict */
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env;
  if (!fid) return 0;
  const FakeID *id = fid;
  if (obj && !strcmp(id->name, "isSuccess")) {
    if (*(uint32_t *)obj == TAG_SNAP_OPEN)
      return ((FakeSnapshotOpenResult *)obj)->success != 0;
    if (*(uint32_t *)obj == TAG_SNAP_COMMIT)
      return ((FakeSnapshotCommitResult *)obj)->success != 0;
  }
  return field_int(id) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy converts reflected Method/Field objects into
// jmethod/jfieldIDs via these. We carry no real reflection, but a non-null opaque
// ID lets the proxy bind and store; an invoked callback routes through act_* and
// no-ops, which is right for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env;
  if (m && *(uint32_t *)m == TAG_ID) return m;
  return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env;
  if (f && *(uint32_t *)f == TAG_ID) return f;
  return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  if (m && name_has(cn, "halfbrick/mortar/NativeGameLib")) {
    for (int i = 0; i < n; ++i) {
      if (m[i].name && m[i].sig &&
          !strcmp(m[i].name, "native_threadEntry") &&
          !strcmp(m[i].sig, "(J)V")) {
        g_mortar_native_thread_entry = (MortarNativeThreadEntryFn)m[i].fn;
      }
    }
  }
  if (m && name_has(cn, "halfbrick/mortar/GooglePlayPurchaseV2Service")) {
    mutexLock(&g_store_callbacks.lock);
    for (int i = 0; i < n; ++i) {
      if (!m[i].name || !m[i].sig || !m[i].fn) continue;
      if (!strcmp(m[i].name, "OnProductsInfo") &&
          !strcmp(m[i].sig,
                  "([Ljava/lang/String;[Ljava/lang/String;[F"
                  "[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;J)V")) {
        g_store_callbacks.products_info = (GooglePlayProductsInfoFn)m[i].fn;
      } else if (!strcmp(m[i].name, "OnPurchaseResult") &&
                 !strcmp(m[i].sig,
                         "(Ljava/lang/String;Ljava/lang/String;ZZ"
                         "Ljava/lang/String;)V")) {
        g_store_callbacks.purchase_result =
            (GooglePlayPurchaseResultFn)m[i].fn;
      } else if (!strcmp(m[i].name, "OnPendingPurchasesEnded") &&
                 !strcmp(m[i].sig, "()V")) {
        g_store_callbacks.pending_ended =
            (GooglePlayPendingEndedFn)m[i].fn;
      } else if (!strcmp(m[i].name, "OnPurchaseRestoreCompleted") &&
                 !strcmp(m[i].sig, "(Ljava/lang/String;)V")) {
        g_store_callbacks.restore_completed =
            (GooglePlayRestoreCompletedFn)m[i].fn;
      }
    }
    mutexUnlock(&g_store_callbacks.lock);
  }
  if (m && name_has(cn, "unity3d/player/UnityPlayer")) {
    for (int i = 0; i < n; ++i) {
      if (!m[i].name) continue;
      if (!strcmp(m[i].name, "nativeSetKeyboardIsVisible"))
        g_unity_keyboard_visible = (UnityKeyboardVisibleFn)m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetInputString"))
        g_unity_input_string = (UnityInputStringFn)m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputClosed"))
        g_unity_soft_input_closed = (UnitySoftInputFn)m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputCanceled"))
        g_unity_soft_input_canceled = (UnitySoftInputFn)m[i].fn;
    }
  }
  return 0;
}

static juint j_UnregisterNatives(void *env, void *cls) {
  (void)env;
  (void)cls;
  return 0;
}

void jni_process_soft_keyboard(void) {
  struct {
    int keyboard_type;
    int secure;
    int character_limit;
    char initial[0x1001];
    char guide[0x101];
  } request;

  mutexLock(&g_soft_keyboard.lock);
  if (!g_soft_keyboard.pending) {
    mutexUnlock(&g_soft_keyboard.lock);
    return;
  }
  request.keyboard_type = g_soft_keyboard.keyboard_type;
  request.secure = g_soft_keyboard.secure;
  request.character_limit = g_soft_keyboard.character_limit;
  snprintf(request.initial, sizeof request.initial, "%s",
           g_soft_keyboard.initial);
  snprintf(request.guide, sizeof request.guide, "%s",
           g_soft_keyboard.guide);
  g_soft_keyboard.pending = 0;
  mutexUnlock(&g_soft_keyboard.lock);

  extern void *fake_unityplayer_thiz;
  if (g_unity_keyboard_visible)
    g_unity_keyboard_visible(fake_env, fake_unityplayer_thiz, 1);

  char result[0x1001];
  snprintf(result, sizeof result, "%s", request.initial);
  SwkbdConfig keyboard;
  Result rc = swkbdCreate(&keyboard, 0);
  if (R_SUCCEEDED(rc)) {
    if (request.secure)
      swkbdConfigMakePresetPassword(&keyboard);
    else
      swkbdConfigMakePresetDefault(&keyboard);
    if (request.keyboard_type == 4 || request.keyboard_type == 5 ||
        request.keyboard_type == 11)
      swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    swkbdConfigSetStringLenMax(&keyboard,
                               (uint32_t)request.character_limit);
    swkbdConfigSetInitialText(&keyboard, request.initial);
    if (request.guide[0])
      swkbdConfigSetGuideText(&keyboard, request.guide);
    rc = swkbdShow(&keyboard, result, sizeof result);
    swkbdClose(&keyboard);
  }

  if (R_SUCCEEDED(rc)) {
    if (g_unity_input_string)
      g_unity_input_string(fake_env, fake_unityplayer_thiz,
                           jni_make_string(result));
    if (g_unity_soft_input_closed)
      g_unity_soft_input_closed(fake_env, fake_unityplayer_thiz);
  } else {
    if (g_unity_soft_input_canceled)
      g_unity_soft_input_canceled(fake_env, fake_unityplayer_thiz);
  }
  if (g_unity_keyboard_visible)
    g_unity_keyboard_visible(fake_env, fake_unityplayer_thiz, 0);
}

void jni_set_snapshot_opened_callback(jni_snapshot_opened_fn callback) {
  mutexLock(&g_snapshot_callbacks.lock);
  g_snapshot_callbacks.opened = callback;
  mutexUnlock(&g_snapshot_callbacks.lock);
}

void jni_set_snapshot_committed_callback(
    jni_snapshot_committed_fn callback) {
  mutexLock(&g_snapshot_callbacks.lock);
  g_snapshot_callbacks.committed = callback;
  mutexUnlock(&g_snapshot_callbacks.lock);
}

void jni_process_deferred_callbacks(void) {
  jni_snapshot_opened_fn snapshot_callback = NULL;
  jni_snapshot_committed_fn commit_callback = NULL;
  int request_id = 0;
  int commit_request_id = 0;
  int commit_success = 0;

  mutexLock(&g_snapshot_callbacks.lock);
  if (g_snapshot_callbacks.opened && g_snapshot_callbacks.open_count) {
    snapshot_callback = g_snapshot_callbacks.opened;
    request_id =
        g_snapshot_callbacks.open_request_ids[g_snapshot_callbacks.open_head];
    g_snapshot_callbacks.open_head =
        (g_snapshot_callbacks.open_head + 1) % MAX_DEFERRED_SNAPSHOT_OPENS;
    --g_snapshot_callbacks.open_count;
  }
  if (g_snapshot_callbacks.committed &&
      g_snapshot_callbacks.commit_count) {
    commit_callback = g_snapshot_callbacks.committed;
    commit_request_id =
        g_snapshot_callbacks.commit_request_ids[
            g_snapshot_callbacks.commit_head];
    commit_success =
        g_snapshot_callbacks.commit_success[g_snapshot_callbacks.commit_head];
    g_snapshot_callbacks.commit_head =
        (g_snapshot_callbacks.commit_head + 1) %
        MAX_DEFERRED_SNAPSHOT_COMMITS;
    --g_snapshot_callbacks.commit_count;
  }
  mutexUnlock(&g_snapshot_callbacks.lock);

  if (snapshot_callback) {
    void *result = make_snapshot_open_result();
    snapshot_callback(
        fake_env,
        jni_make_object("com/halfbrick/mortar/GooglePlaySnapshotManager"),
        request_id, result);
    delete_local(result);
  }

  if (commit_callback) {
    void *result = make_snapshot_commit_result(commit_success);
    commit_callback(
        fake_env,
        jni_make_object("com/halfbrick/mortar/GooglePlaySnapshotManager"),
        commit_request_id, result);
    delete_local(result);
  }

  GooglePlayProductsInfoFn products_info = NULL;
  GooglePlayPurchaseResultFn purchase_result = NULL;
  GooglePlayPendingEndedFn pending_ended = NULL;
  GooglePlayRestoreCompletedFn restore_completed = NULL;
  static ProductInfoRequest info_request;
  char purchase_id[MAX_STORE_PRODUCT_ID] = {0};
  uint64_t token_id = 0;
  int have_info = 0, have_purchase = 0;
  int have_pending_end = 0, have_restore_complete = 0;

  mutexLock(&g_store_callbacks.lock);
  if (g_store_callbacks.products_info && g_store_callbacks.info_count) {
    products_info = g_store_callbacks.products_info;
    info_request = g_store_callbacks.info[g_store_callbacks.info_head];
    g_store_callbacks.info_head =
        (g_store_callbacks.info_head + 1) % MAX_PRODUCT_INFO_REQUESTS;
    --g_store_callbacks.info_count;
    have_info = 1;
  }
  if (g_store_callbacks.purchase_result &&
      g_store_callbacks.purchase_count) {
    purchase_result = g_store_callbacks.purchase_result;
    snprintf(purchase_id, sizeof purchase_id, "%s",
             g_store_callbacks.purchase_ids[g_store_callbacks.purchase_head]);
    g_store_callbacks.purchase_head =
        (g_store_callbacks.purchase_head + 1) % MAX_DEFERRED_PURCHASES;
    --g_store_callbacks.purchase_count;
    token_id = g_store_callbacks.next_token++;
    have_purchase = 1;
  }
  if (g_store_callbacks.pending_ended &&
      g_store_callbacks.pending_end_count) {
    pending_ended = g_store_callbacks.pending_ended;
    --g_store_callbacks.pending_end_count;
    have_pending_end = 1;
  }
  if (g_store_callbacks.restore_completed &&
      g_store_callbacks.restore_complete_count) {
    restore_completed = g_store_callbacks.restore_completed;
    --g_store_callbacks.restore_complete_count;
    have_restore_complete = 1;
  }
  mutexUnlock(&g_store_callbacks.lock);

  if (have_info) {
    const int count = (int)info_request.product_count;
    void *ids = j_NewObjectArray(fake_env, count,
                                 intern_class("java/lang/String"), NULL);
    void *titles = j_NewObjectArray(fake_env, count,
                                    intern_class("java/lang/String"), NULL);
    void *prices = j_NewFloatArray(fake_env, count);
    void *formatted = j_NewObjectArray(fake_env, count,
                                       intern_class("java/lang/String"), NULL);
    void *currencies = j_NewObjectArray(fake_env, count,
                                        intern_class("java/lang/String"), NULL);
    FakePriArray *price_array = prices;
    float *price_values = price_array && price_array->tag == TAG_PRIARR
                              ? (float *)price_array->data : NULL;
    for (int i = 0; i < count; ++i) {
      j_SetObjectArrayElement(fake_env, ids, i,
                              jni_make_string(info_request.product_ids[i]));
      j_SetObjectArrayElement(fake_env, titles, i,
                              jni_make_string("Free Offline Purchase"));
      if (price_values) price_values[i] = 0.0f;
      j_SetObjectArrayElement(fake_env, formatted, i,
                              jni_make_string("FREE"));
      j_SetObjectArrayElement(fake_env, currencies, i,
                              jni_make_string("USD"));
    }
    products_info(
        fake_env,
        jni_make_object("com/halfbrick/mortar/GooglePlayPurchaseV2Service"),
        ids, titles, prices, formatted, currencies, NULL,
        info_request.request_id);
    delete_local(ids);
    delete_local(titles);
    delete_local(prices);
    delete_local(formatted);
    delete_local(currencies);
  }

  if (have_purchase) {
    char token[64];
    snprintf(token, sizeof token, "nx-free-%llu",
             (unsigned long long)token_id);
    purchase_result(
        fake_env,
        jni_make_object("com/halfbrick/mortar/GooglePlayPurchaseV2Service"),
        jni_make_string(purchase_id), jni_make_string(token), 0, 0, NULL);
  }

  if (have_pending_end) {
    pending_ended(
        fake_env,
        jni_make_object("com/halfbrick/mortar/GooglePlayPurchaseV2Service"));
  }

  if (have_restore_complete) {
    restore_completed(
        fake_env,
        jni_make_object("com/halfbrick/mortar/GooglePlayPurchaseV2Service"),
        NULL);
  }
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* Accessors used by unity_jni.c and unity_input.c to read
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);
  mutexInit(&g_soft_keyboard.lock);
  mutexInit(&g_snapshot_callbacks.lock);
  mutexInit(&g_store_callbacks.lock);
  mutexInit(&g_local_keystore.lock);
  g_snapshot_callbacks.next_request_id = 1;
  g_snapshot_callbacks.last_write_ok = 1;
  g_store_callbacks.next_token = 1;

  jni_fill_unimpl(env_table);

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[178] = (void *)j_NewShortArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_UnregisterNatives;
  env_table[217] = (void *)j_MonitorNoop;             // MonitorEnter
  env_table[218] = (void *)j_MonitorNoop;             // MonitorExit
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}

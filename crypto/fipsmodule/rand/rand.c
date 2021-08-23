/* Copyright (c) 2014, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/rand.h>

#include <assert.h>
#include <limits.h>
#include <string.h>

#if defined(BORINGSSL_FIPS)
#include <unistd.h>
#endif

#include <openssl/chacha.h>
#include <openssl/cpu.h>
#include <openssl/mem.h>
#include <openssl/type_check.h>

#include "internal.h"
#include "fork_detect.h"
#include "../../internal.h"
#include "../delocate.h"


// It's assumed that the operating system always has an unfailing source of
// entropy which is accessed via |CRYPTO_sysrand[_for_seed]|. (If the operating
// system entropy source fails, it's up to |CRYPTO_sysrand| to abort the
// process—we don't try to handle it.)
//
// In addition, the hardware may provide a low-latency RNG. Intel's rdrand
// instruction is the canonical example of this. When a hardware RNG is
// available we don't need to worry about an RNG failure arising from fork()ing
// the process or moving a VM, so we can keep thread-local RNG state and use it
// as an additional-data input to CTR-DRBG.
//
// (We assume that the OS entropy is safe from fork()ing and VM duplication.
// This might be a bit of a leap of faith, esp on Windows, but there's nothing
// that we can do about it.)

// kReseedInterval is the number of generate calls made to CTR-DRBG before
// reseeding.
static const unsigned kReseedInterval = 4096;

// CRNGT_BLOCK_SIZE is the number of bytes in a “block” for the purposes of the
// continuous random number generator test in FIPS 140-2, section 4.9.2.
#define CRNGT_BLOCK_SIZE 16

// rand_thread_state contains the per-thread state for the RNG.
struct rand_thread_state {
  CTR_DRBG_STATE drbg;
  uint64_t fork_generation;
  // calls is the number of generate calls made on |drbg| since it was last
  // (re)seeded. This is bound by |kReseedInterval|.
  unsigned calls;
  // last_block_valid is non-zero iff |last_block| contains data from
  // |get_seed_entropy|.
  int last_block_valid;

#if defined(BORINGSSL_FIPS)
  // last_block contains the previous block from |get_seed_entropy|.
  uint8_t last_block[CRNGT_BLOCK_SIZE];
  // next and prev form a NULL-terminated, double-linked list of all states in
  // a process.
  struct rand_thread_state *next, *prev;
#endif
};

// |CTR_DRBG_AES_128_ENTROPY_LEN| and |CTR_DRBG_AES_256_ENTROPY_LEN| are used as
// upper bounds for indexing into |CTR_DRBG_MAX_ENTROPY_LEN| length arrays.
// Ensure there are no OOB events.
OPENSSL_STATIC_ASSERT(CTR_DRBG_MAX_ENTROPY_LEN >= CTR_DRBG_AES_128_ENTROPY_LEN,
  CTR_DRBG_MAX_ENTROPY_LEN_is_not_max)
OPENSSL_STATIC_ASSERT(CTR_DRBG_MAX_ENTROPY_LEN >= CTR_DRBG_AES_256_ENTROPY_LEN,
  CTR_DRBG_MAX_ENTROPY_LEN_is_not_max)

static size_t ctr_drbg_get_entropy_length(ctr_drbg_key_len_t ctr_drbg_key_len) {
  switch (ctr_drbg_key_len) {
    case CTR_DRBG_AES_128:
      return CTR_DRBG_AES_128_ENTROPY_LEN;
    case CTR_DRBG_AES_256:
      return CTR_DRBG_AES_256_ENTROPY_LEN;
    default:
      fprintf(stderr, "CTR-DRBG key type not supported!\n");
      abort();
  }
}

// |CTR_DRBG_AES_128_KEY_LEN| and |CTR_DRBG_AES_256_KEY_LEN| are used as
// upper bounds for indexing into |CTR_DRBG_MAX_AES_KEY_LEN| length arrays.
// Ensure there are no OOB events.
OPENSSL_STATIC_ASSERT(CTR_DRBG_MAX_AES_KEY_LEN >= CTR_DRBG_AES_128_KEY_LEN,
  CTR_DRBG_MAX_KEY_LEN_is_not_max)
OPENSSL_STATIC_ASSERT(CTR_DRBG_MAX_AES_KEY_LEN >= CTR_DRBG_AES_256_KEY_LEN,
  CTR_DRBG_MAX_KEY_LEN_is_not_max)

static size_t ctr_drbg_get_key_length(ctr_drbg_key_len_t ctr_drbg_key_len) {
  switch (ctr_drbg_key_len) {
    case CTR_DRBG_AES_128:
      return CTR_DRBG_AES_128_KEY_LEN;
    case CTR_DRBG_AES_256:
      return CTR_DRBG_AES_256_KEY_LEN;
    default:
      fprintf(stderr, "CTR-DRBG key type not supported!\n");
      abort();
  }
}

#if defined(BORINGSSL_FIPS)
// thread_states_list is the head of a linked-list of all |rand_thread_state|
// objects in the process, one per thread. This is needed because FIPS requires
// that they be zeroed on process exit, but thread-local destructors aren't
// called when the whole process is exiting.
DEFINE_BSS_GET(struct rand_thread_state *, thread_states_list)
DEFINE_STATIC_MUTEX(thread_states_list_lock)
DEFINE_STATIC_MUTEX(state_clear_all_lock)

static void rand_thread_state_clear_all(void) __attribute__((destructor));
static void rand_thread_state_clear_all(void) {
  CRYPTO_STATIC_MUTEX_lock_write(thread_states_list_lock_bss_get());
  CRYPTO_STATIC_MUTEX_lock_write(state_clear_all_lock_bss_get());
  for (struct rand_thread_state *cur = *thread_states_list_bss_get();
       cur != NULL; cur = cur->next) {
    CTR_DRBG_clear(&cur->drbg);
  }
  // The locks are deliberately left locked so that any threads that are still
  // running will hang if they try to call |RAND_bytes|.
}
#endif

// rand_thread_state_free frees a |rand_thread_state|. This is called when a
// thread exits.
static void rand_thread_state_free(void *state_in) {
  struct rand_thread_state *state = state_in;

  if (state_in == NULL) {
    return;
  }

#if defined(BORINGSSL_FIPS)
  CRYPTO_STATIC_MUTEX_lock_write(thread_states_list_lock_bss_get());

  if (state->prev != NULL) {
    state->prev->next = state->next;
  } else {
    *thread_states_list_bss_get() = state->next;
  }

  if (state->next != NULL) {
    state->next->prev = state->prev;
  }

  CRYPTO_STATIC_MUTEX_unlock_write(thread_states_list_lock_bss_get());

  CTR_DRBG_clear(&state->drbg);
#endif

  OPENSSL_free(state);
}

#if defined(OPENSSL_X86_64) && !defined(OPENSSL_NO_ASM) && \
    !defined(BORINGSSL_UNSAFE_DETERMINISTIC_MODE)

// rdrand maximum retries as suggested by:
// Intel® Digital Random Number Generator (DRNG) Software Implementation Guide
// Revision 2.1
// https://software.intel.com/content/www/us/en/develop/articles/intel-digital-random-number-generator-drng-software-implementation-guide.html
#define RDRAND_MAX_RETRIES 10

OPENSSL_STATIC_ASSERT(RDRAND_MAX_RETRIES > 0, rdrand_max_retries_must_be_positive)
#define CALL_RDRAND_WITH_RETRY(rdrand_func, fail_ret_value)       \
    for (size_t tries = 0; tries < RDRAND_MAX_RETRIES; tries++) { \
      if ((rdrand_func) == 1) {                                   \
        break;                                                    \
      }                                                           \
      else if (tries == RDRAND_MAX_RETRIES - 1) {                 \
        return fail_ret_value;                                    \
      }                                                           \
    }

// rdrand should only be called if either |have_rdrand| or |have_fast_rdrand|
// returned true.
static int rdrand(uint8_t *buf, const size_t len) {
  const size_t len_multiple8 = len & ~7;
  CALL_RDRAND_WITH_RETRY(CRYPTO_rdrand_multiple8_buf(buf, len_multiple8), 0)
  const size_t remainder = len - len_multiple8;

  if (remainder != 0) {
    assert(remainder < 8);

    uint8_t rand_buf[8];
    CALL_RDRAND_WITH_RETRY(CRYPTO_rdrand(rand_buf), 0)
    OPENSSL_memcpy(buf + len_multiple8, rand_buf, remainder);
  }

  return 1;
}

#else

static int rdrand(uint8_t *buf, size_t len) {
  return 0;
}

#endif

#if defined(BORINGSSL_FIPS)

void CRYPTO_get_seed_entropy(uint8_t *out_entropy, size_t out_entropy_len,
                             int *out_used_cpu) {
  *out_used_cpu = 0;
  if (have_rdrand() && rdrand(out_entropy, out_entropy_len)) {
    *out_used_cpu = 1;
  } else {
    CRYPTO_sysrand_for_seed(out_entropy, out_entropy_len);
  }

#if defined(BORINGSSL_FIPS_BREAK_CRNG)
  // This breaks the "continuous random number generator test" defined in FIPS
  // 140-2, section 4.9.2, and implemented in |rand_get_seed|.
  OPENSSL_memset(out_entropy, 0, out_entropy_len);
#endif
}

// In passive entropy mode, entropy is supplied from outside of the module via
// |RAND_load_entropy| and is stored in global instance of the following
// structure.

struct entropy_buffer {
  // bytes contains entropy suitable for seeding a DRBG.
  uint8_t bytes[CTR_DRBG_MAX_ENTROPY_LEN * BORINGSSL_FIPS_OVERREAD];
  // bytes_valid indicates the number of bytes of |bytes| that contain valid
  // data.
  size_t bytes_valid;
  // from_cpu is true if any of the contents of |bytes| were obtained directly
  // from the CPU.
  int from_cpu;
};

DEFINE_BSS_GET(struct entropy_buffer, entropy_buffer);
DEFINE_STATIC_MUTEX(entropy_buffer_lock);

void RAND_load_entropy(const uint8_t *entropy, size_t entropy_len,
                       int from_cpu) {
  struct entropy_buffer *const buffer = entropy_buffer_bss_get();

  CRYPTO_STATIC_MUTEX_lock_write(entropy_buffer_lock_bss_get());
  const size_t space = sizeof(buffer->bytes) - buffer->bytes_valid;
  if (entropy_len > space) {
    entropy_len = space;
  }

  OPENSSL_memcpy(&buffer->bytes[buffer->bytes_valid], entropy, entropy_len);
  buffer->bytes_valid += entropy_len;
  buffer->from_cpu |= from_cpu && (entropy_len != 0);
  CRYPTO_STATIC_MUTEX_unlock_write(entropy_buffer_lock_bss_get());
}

// get_seed_entropy fills |out_entropy_len| bytes of |out_entropy| from the
// global |entropy_buffer|.
static void get_seed_entropy(uint8_t *out_entropy, size_t out_entropy_len,
                             int *out_used_cpu) {
  struct entropy_buffer *const buffer = entropy_buffer_bss_get();
  if (out_entropy_len > sizeof(buffer->bytes)) {
    abort();
  }

  CRYPTO_STATIC_MUTEX_lock_write(entropy_buffer_lock_bss_get());
  while (buffer->bytes_valid < out_entropy_len) {
    CRYPTO_STATIC_MUTEX_unlock_write(entropy_buffer_lock_bss_get());
    RAND_need_entropy(out_entropy_len - buffer->bytes_valid);
    CRYPTO_STATIC_MUTEX_lock_write(entropy_buffer_lock_bss_get());
  }

  *out_used_cpu = buffer->from_cpu;
  OPENSSL_memcpy(out_entropy, buffer->bytes, out_entropy_len);
  OPENSSL_memmove(buffer->bytes, &buffer->bytes[out_entropy_len],
                  buffer->bytes_valid - out_entropy_len);
  buffer->bytes_valid -= out_entropy_len;
  if (buffer->bytes_valid == 0) {
    buffer->from_cpu = 0;
  }

  CRYPTO_STATIC_MUTEX_unlock_write(entropy_buffer_lock_bss_get());
}

// rand_get_seed fills |seed| with entropy and sets |*out_used_cpu| to one if
// that entropy came directly from the CPU and zero otherwise.
static void rand_get_seed(struct rand_thread_state *state,
                          uint8_t seed[CTR_DRBG_MAX_ENTROPY_LEN],
                          int *out_used_cpu,
                          size_t entropy_len) {
  if (!state->last_block_valid) {
    int unused;
    get_seed_entropy(state->last_block, sizeof(state->last_block), &unused);
    state->last_block_valid = 1;
  }

  uint8_t entropy[CTR_DRBG_MAX_ENTROPY_LEN * BORINGSSL_FIPS_OVERREAD];
  size_t entropy_len_including_overread = entropy_len * BORINGSSL_FIPS_OVERREAD;

  get_seed_entropy(entropy, entropy_len_including_overread, out_used_cpu);

  // See FIPS 140-2, section 4.9.2. This is the “continuous random number
  // generator test” which causes the program to randomly abort. Hopefully the
  // rate of failure is small enough not to be a problem in practice.
  if (CRYPTO_memcmp(state->last_block, entropy, CRNGT_BLOCK_SIZE) == 0) {
    fprintf(stderr, "CRNGT failed.\n");
    BORINGSSL_FIPS_abort();
  }

  // |entropy_len_including_overread| is one of
  // |CTR_DRBG_AES_{128,256}_ENTROPY_LEN| * |BORINGSSL_FIPS_OVERREAD|.
  OPENSSL_STATIC_ASSERT((CTR_DRBG_AES_128_ENTROPY_LEN * BORINGSSL_FIPS_OVERREAD) % CRNGT_BLOCK_SIZE == 0, _)
  OPENSSL_STATIC_ASSERT((CTR_DRBG_AES_256_ENTROPY_LEN * BORINGSSL_FIPS_OVERREAD) % CRNGT_BLOCK_SIZE == 0, _)
  for (size_t i = CRNGT_BLOCK_SIZE; i < entropy_len_including_overread;
       i += CRNGT_BLOCK_SIZE) {
    if (CRYPTO_memcmp(entropy + i - CRNGT_BLOCK_SIZE, entropy + i,
                      CRNGT_BLOCK_SIZE) == 0) {
      fprintf(stderr, "CRNGT failed.\n");
      BORINGSSL_FIPS_abort();
    }
  }
  OPENSSL_memcpy(state->last_block,
                 entropy + entropy_len_including_overread - CRNGT_BLOCK_SIZE,
                 CRNGT_BLOCK_SIZE);

  OPENSSL_memcpy(seed, entropy, entropy_len);

  for (size_t i = 1; i < BORINGSSL_FIPS_OVERREAD; i++) {
    for (size_t j = 0; j < entropy_len; j++) {
      seed[j] ^= entropy[(entropy_len * i) + j];
    }
  }
}

#else

// rand_get_seed fills |seed| with entropy and sets |*out_used_cpu| to one if
// that entropy came directly from the CPU and zero otherwise.
static void rand_get_seed(struct rand_thread_state *state,
                          uint8_t seed[CTR_DRBG_MAX_ENTROPY_LEN],
                          int *out_used_cpu,
                          size_t entropy_len) {

  // If not in FIPS mode, we don't overread from the system entropy source and
  // we don't depend only on the hardware RDRAND.
  CRYPTO_sysrand(seed, entropy_len);
  *out_used_cpu = 0;
}

#endif

// We want to ensure that |CTR_DRBG_MAX_AES_KEY_LEN| is always 32, because this
// is assumed higher up the stack e.g. big number library module (see
// |bn_rand_range_words()|). But we do not want to pollute the rest of the code
// with includes and |CTR_DRBG_MAX_AES_KEY_LEN|. The assert below will catch
// if the max value ever changes.
OPENSSL_STATIC_ASSERT(CTR_DRBG_MAX_AES_KEY_LEN == 32,
  CTR_DRBG_AES_max_key_size_is_not_32)

void RAND_bytes_with_additional_data(uint8_t *out, size_t out_len,
                                     const uint8_t user_additional_data[CTR_DRBG_MAX_AES_KEY_LEN],
                                     ctr_drbg_key_len_t ctr_drbg_key_len) {
  if (out_len == 0) {
    return;
  }

  size_t entropy_len = ctr_drbg_get_entropy_length(ctr_drbg_key_len);
  size_t aes_key_len = ctr_drbg_get_key_length(ctr_drbg_key_len);

  const uint64_t fork_generation = CRYPTO_get_fork_generation();

  // Additional data is mixed into every CTR-DRBG call to protect, as best we
  // can, against forks & VM clones. We do not over-read this information and
  // don't reseed with it so, from the point of view of FIPS, this doesn't
  // provide “prediction resistance”. But, in practice, it does.
  uint8_t additional_data[CTR_DRBG_MAX_AES_KEY_LEN];
  // Intel chips have fast RDRAND instructions while, in other cases, RDRAND can
  // be _slower_ than a system call.
  if (!have_fast_rdrand() ||
      !rdrand(additional_data, aes_key_len)) {
    // Without a hardware RNG to save us from address-space duplication, the OS
    // entropy is used. This can be expensive (one read per |RAND_bytes| call)
    // and so is disabled when we have fork detection, or if the application has
    // promised not to fork.
    if (fork_generation != 0 || rand_fork_unsafe_buffering_enabled()) {
      OPENSSL_memset(additional_data, 0, aes_key_len);
    } else if (!have_rdrand()) {
      // No alternative so block for OS entropy.
      CRYPTO_sysrand(additional_data, aes_key_len);
    } else if (!CRYPTO_sysrand_if_available(additional_data, aes_key_len) &&
               !rdrand(additional_data, aes_key_len)) {
      // RDRAND failed: block for OS entropy.
      CRYPTO_sysrand(additional_data, aes_key_len);
    }
  }

  for (size_t i = 0; i < aes_key_len; i++) {
    additional_data[i] ^= user_additional_data[i];
  }

  struct rand_thread_state stack_state;
  struct rand_thread_state *state =
      CRYPTO_get_thread_local(OPENSSL_THREAD_LOCAL_RAND);

  if (state == NULL) {
    state = OPENSSL_malloc(sizeof(struct rand_thread_state));
    if (state == NULL ||
        !CRYPTO_set_thread_local(OPENSSL_THREAD_LOCAL_RAND, state,
                                 rand_thread_state_free)) {
      // If the system is out of memory, use an ephemeral state on the
      // stack.
      state = &stack_state;
    }

    state->last_block_valid = 0;
    uint8_t seed[CTR_DRBG_MAX_ENTROPY_LEN];
    int used_cpu;
    rand_get_seed(state, seed, &used_cpu, entropy_len);

    uint8_t personalization[CTR_DRBG_MAX_ENTROPY_LEN];
    size_t personalization_len = 0;
#if defined(OPENSSL_URANDOM)
    // If we used RDRAND, also opportunistically read from the system. This
    // avoids solely relying on the hardware once the entropy pool has been
    // initialized.
    if (used_cpu) {
      if (CRYPTO_sysrand_if_available(personalization, entropy_len)) {
        personalization_len = entropy_len;
      }
    }
#endif

    if (!CTR_DRBG_init(&state->drbg, seed, personalization,
                       personalization_len, aes_key_len)) {
      abort();
    }
    state->calls = 0;
    state->fork_generation = fork_generation;

#if defined(BORINGSSL_FIPS)
    if (state != &stack_state) {
      CRYPTO_STATIC_MUTEX_lock_write(thread_states_list_lock_bss_get());
      struct rand_thread_state **states_list = thread_states_list_bss_get();
      state->next = *states_list;
      if (state->next != NULL) {
        state->next->prev = state;
      }
      state->prev = NULL;
      *states_list = state;
      CRYPTO_STATIC_MUTEX_unlock_write(thread_states_list_lock_bss_get());
    }
#endif
  }

  if (state->calls >= kReseedInterval ||
      state->fork_generation != fork_generation) {
    uint8_t seed[CTR_DRBG_MAX_ENTROPY_LEN];
    int used_cpu;
    rand_get_seed(state, seed, &used_cpu, entropy_len);
#if defined(BORINGSSL_FIPS)
    // Take a read lock around accesses to |state->drbg|. This is needed to
    // avoid returning bad entropy if we race with
    // |rand_thread_state_clear_all|.
    //
    // This lock must be taken after any calls to |CRYPTO_sysrand| to avoid a
    // bug on ppc64le. glibc may implement pthread locks by wrapping user code
    // in a hardware transaction, but, on some older versions of glibc and the
    // kernel, syscalls made with |syscall| did not abort the transaction.
    CRYPTO_STATIC_MUTEX_lock_read(state_clear_all_lock_bss_get());
#endif
    if (!CTR_DRBG_reseed(&state->drbg, seed, NULL, 0)) {
      abort();
    }
    state->calls = 0;
    state->fork_generation = fork_generation;
  } else {
#if defined(BORINGSSL_FIPS)
    CRYPTO_STATIC_MUTEX_lock_read(state_clear_all_lock_bss_get());
#endif
  }

  int first_call = 1;
  while (out_len > 0) {
    size_t todo = out_len;
    if (todo > CTR_DRBG_MAX_GENERATE_LENGTH) {
      todo = CTR_DRBG_MAX_GENERATE_LENGTH;
    }

    if (!CTR_DRBG_generate(&state->drbg, out, todo, additional_data,
                           first_call ? aes_key_len : 0)) {
      abort();
    }

    out += todo;
    out_len -= todo;
    // Though we only check before entering the loop, this cannot add enough to
    // overflow a |size_t|.
    state->calls++;
    first_call = 0;
  }

  if (state == &stack_state) {
    CTR_DRBG_clear(&state->drbg);
  }

#if defined(BORINGSSL_FIPS)
  CRYPTO_STATIC_MUTEX_unlock_read(state_clear_all_lock_bss_get());
#endif
}

int RAND_bytes(uint8_t *out, size_t out_len) {
  static const uint8_t kZeroAdditionalData[CTR_DRBG_MAX_AES_KEY_LEN] = {0};
  RAND_bytes_with_additional_data(out, out_len, kZeroAdditionalData, CTR_DRBG_AES_256);
  return 1;
}

int RAND_pseudo_bytes(uint8_t *buf, size_t len) {
  return RAND_bytes(buf, len);
}

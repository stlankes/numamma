/* -*- c-file-style: "GNU" -*- */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <string.h>
#include <execinfo.h>

#include "memory.h"
#include "mem_analyzer.h"

#define EZTRACE_PROTECT if(malloc_protect_on == 0)

//#define DEBUG 1

#if DEBUG
#define FUNCTION_ENTRY printf("%s\n", __FUNCTION__)
#else
#define FUNCTION_ENTRY //printf("%s\n", __FUNCTION__)
#endif

/* set to 1 when all the hooks are set.
 * This is useful in order to avoid recursive calls
 */
static int __memory_initialized = 0;

/* todo: also implement mmap and munmap ?
 */
void* (*libcalloc)(size_t nmemb, size_t size) = NULL;
void* (*libmalloc)(size_t size) = NULL;
void (*libfree)(void *ptr) = NULL;
void* (*librealloc)(void *ptr, size_t size) = NULL;
int  (*libpthread_create) (pthread_t * thread, const pthread_attr_t * attr,
			   void *(*start_routine) (void *), void *arg);
void (*libpthread_exit) (void *thread_return);

static int malloc_protect_on = 0;

void print_backtrace() {
#define BT_BUF_SIZE 100
  int j, nptrs;
  void *buffer[BT_BUF_SIZE];
  char **strings;

  nptrs = backtrace(buffer, BT_BUF_SIZE);
  printf("backtrace() returned %d addresses\n", nptrs);
  printf("-------------------\n");
  /* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
     would produce similar output to the following: */

  strings = backtrace_symbols(buffer, nptrs);
  if (strings == NULL) {
    perror("backtrace_symbols");
    exit(EXIT_FAILURE);
  }

  for (j = 0; j < nptrs; j++)
    printf("%s\n", strings[j]);
  printf("-------------------\n");

  free(strings);
}

/* Custom malloc function. It is used when libmalloc=NULL (e.g. during startup)
 * This function is not thread-safe and is very likely to be bogus, so use with
 * caution
 */
static void* hand_made_malloc(size_t size) {
  /* allocate a 1MB buffer */
#define POOL_SIZE (1024 * 1024)
  static char mem[POOL_SIZE] = {'\0'};

  /* since this function is only used before we found libmalloc, there's no
   * fancy memory management mechanism (block reuse, etc.)
   */
  static char* next_slot = &mem[0];
  static int total_alloc = 0;

  if (libmalloc)
    /* let's use the real malloc */
    return malloc(size);

  struct mem_block_info *p_block = NULL;
  INIT_MEM_INFO(p_block, next_slot, size, 1);

  p_block->mem_type = MEM_TYPE_CUSTOM_MALLOC;
  total_alloc += size;
  next_slot = next_slot + p_block->total_size;

  return p_block->u_ptr;
}

void* malloc(size_t size) {
  /* if memory_init hasn't been called yet, we need to get libc's malloc
   * address
   */
  if (!libmalloc) {
    if (malloc_protect_on)
      /* protection flag says that malloc is already trying to retrieve the
       * address of malloc.
       * If we call dlsym now, there will be an infinite recursion, so let's
       * allocate memory 'by hand'
       */
      return hand_made_malloc(size);

    /* set the protection flag and retrieve the address of malloc.
     * If dlsym calls malloc, memory will be allocated 'by hand'
     */
    malloc_protect_on = 1;
    libmalloc = dlsym(RTLD_NEXT, "malloc");
    char* error;
    if ((error = dlerror()) != NULL) {
      fputs(error, stderr);
      exit(1);
    }
    /* it is now safe to call libmalloc */
    malloc_protect_on = 0;
  }

  EZTRACE_PROTECT {
    malloc_protect_on = 1;
    //    FUNCTION_ENTRY;
#if DEBUG
    printf("malloc(%d) ", size);
#endif
    void* pptr = libmalloc(size + HEADER_SIZE + TAIL_SIZE);
    struct mem_block_info *p_block = NULL;
    INIT_MEM_INFO(p_block, pptr, size, 1);
    p_block->mem_type = MEM_TYPE_MALLOC;

#if 0
    /* for debugging purpose only */
    uint32_t* canary = p_block->u_ptr-sizeof(uint32_t);
    if(*canary != CANARY_PATTERN) {
      fprintf(stderr, "warning: canary = %x instead of %x\n", *canary, CANARY_PATTERN);
    }
#endif

    ma_record_malloc(p_block);
#if DEBUG
    printf("-> %p\n", p_block->u_ptr);
#endif
#if 0
    print_backtrace();
#endif
    malloc_protect_on = 0;
    return p_block->u_ptr;
  }
  return libmalloc(size);
}

void* realloc(void *ptr, size_t size) {

  /* if ptr is NULL, realloc behaves like malloc */
  if (!ptr)
    return malloc(size);

  /* if size=0 and ptr isn't NULL, realloc behaves like free */
  if (!size && ptr) {
    free(ptr);
    return NULL;
  }

  FUNCTION_ENTRY;

  if (!librealloc) {
    librealloc = dlsym(RTLD_NEXT, "realloc");
    char* error;
    if ((error = dlerror()) != NULL) {
      fputs(error, stderr);
      exit(1);
    }
  }

  if (!CANARY_OK(ptr)) {
    /* we didn't malloc'ed this buffer */
    return librealloc(ptr, size);
  }

  EZTRACE_PROTECT {
    malloc_protect_on = 1;
    struct mem_block_info *p_block;
    USER_PTR_TO_BLOCK_INFO(ptr, p_block);
    size_t old_size = p_block->size;
    size_t header_size = p_block->total_size - p_block->size;

    if (p_block->mem_type != MEM_TYPE_MALLOC) {
      fprintf(
	  stderr,
	  "Warning: realloc a ptr that was allocated by hand_made_malloc\n");
    }

    void *old_addr= p_block->u_ptr;
    void *pptr = librealloc(p_block->p_ptr, size + header_size);

    if (!p_block) {
      return NULL;
    }

    INIT_MEM_INFO(p_block, pptr, size + header_size, 1);

    p_block->mem_type = MEM_TYPE_MALLOC;
    void *new_addr= p_block->u_ptr;
    ma_update_buffer_address(old_addr, new_addr);

    malloc_protect_on = 0;
    return p_block->u_ptr;
  }

  return librealloc(ptr, size);
}

void* calloc(size_t nmemb, size_t size) {
  if (!libcalloc) {
    void* ret = hand_made_malloc(nmemb * size);
    if (ret) {
      memset(ret, 0, nmemb * size);
    }
    return ret;
  }
  FUNCTION_ENTRY;

  EZTRACE_PROTECT {
    malloc_protect_on = 1;
    /* compute the number of blocks for header */
    int nb_memb_header = (HEADER_SIZE  + TAIL_SIZE)/ size;
    if (size * nb_memb_header < HEADER_SIZE + TAIL_SIZE)
      nb_memb_header++;

    /* allocate buffer + header */
    void* p_ptr = libcalloc(nmemb + nb_memb_header, size);

    struct mem_block_info *p_block = NULL;
    INIT_MEM_INFO(p_block, p_ptr, nmemb, size);
    p_block->mem_type = MEM_TYPE_MALLOC;

#if 0
    /* for debugging purpose only */
    uint32_t* canary = p_block->u_ptr-sizeof(uint32_t);
    if(*canary != CANARY_PATTERN) {
      fprintf(stderr, "warning: canary = %x instead of %x\n", *canary, CANARY_PATTERN);
    }
#endif
    ma_record_malloc(p_block);
    malloc_protect_on = 0;
    return p_block->u_ptr;
  }
  return libcalloc(nmemb, size);
}

void free(void* ptr) {

  if (!libfree) {
    libfree = dlsym(RTLD_NEXT, "free");
    char* error;
    if ((error = dlerror()) != NULL) {
      fputs(error, stderr);
      exit(1);
    }
  }
  if (!ptr) {
    libfree(ptr);
    return;
  }

  /* first, check wether we malloc'ed the buffer */
  if (!CANARY_OK(ptr)) {
    /* we didn't malloc this buffer */
    libfree(ptr);
    return;
  }

  /* retrieve the block information and free it */
  EZTRACE_PROTECT {
    malloc_protect_on = 1;
    struct mem_block_info *p_block;
    USER_PTR_TO_BLOCK_INFO(ptr, p_block);

#if DEBUG
    printf("free(%p)\n", ptr);
#endif
  //  FUNCTION_ENTRY;

#if 1
    if(!TAIL_CANARY_OK(p_block)) {
      fprintf(stderr, "Warning: tail canary erased :'( (%x instead of %x)\n", p_block->tail_block->canary, CANARY_PATTERN);
      abort();
    }
#endif
    if (p_block->mem_type == MEM_TYPE_MALLOC) {
      ma_record_free(p_block);
      libfree(p_block->p_ptr);
    } else {
      /* the buffer was allocated by hand_made_malloc, there's nothing to free */
    }
    malloc_protect_on = 0;
    return;
  }
  libfree(ptr);
}

/* Internal structure used for transmitting the function and argument
 * during pthread_create.
 */
struct __pthread_create_info_t {
  void *(*func)(void *);
  void *arg;
};


/* Invoked by pthread_create on the new thread */
static void *
__pthread_new_thread(void *arg) {
  struct __pthread_create_info_t *p_arg = (struct __pthread_create_info_t*) arg;
  void *(*f)(void *) = p_arg->func;
  void *__arg = p_arg->arg;
  free(p_arg);
  ma_thread_init();
  void *res = (*f)(__arg);
  ma_thread_finalize();
  return res;
}

int
pthread_create (pthread_t *__restrict thread,
		const pthread_attr_t *__restrict attr,
		void *(*start_routine) (void *),
		void *__restrict arg)
{
  FUNCTION_ENTRY;
  struct __pthread_create_info_t * __args =
    (struct __pthread_create_info_t*) malloc(
	sizeof(struct __pthread_create_info_t));
  __args->func = start_routine;
  __args->arg = arg;

  if (!libpthread_create) {
    libpthread_create = dlsym(RTLD_NEXT, "pthread_create");
  }

  /* We do not call directly start_routine since we could not get the actual creation timestamp of
   * the thread. Let's invoke __pthread_new_thread that will PROF_EVENT the thread and call
   * start_routine.
   */
  int retval = libpthread_create(thread, attr, __pthread_new_thread, __args);
  return retval;
}

void pthread_exit(void *thread_return) {
  FUNCTION_ENTRY;
  libpthread_exit(thread_return);
  __builtin_unreachable();
}

static void __memory_init(void) __attribute__ ((constructor));
static void __memory_init(void) {
  malloc_protect_on = 1;

  libmalloc = dlsym(RTLD_NEXT, "malloc");
  libcalloc = dlsym(RTLD_NEXT, "calloc");
  librealloc = dlsym(RTLD_NEXT, "realloc");
  libfree = dlsym(RTLD_NEXT, "free");
  libpthread_create = dlsym(RTLD_NEXT, "pthread_create");
  libpthread_exit = dlsym(RTLD_NEXT, "pthread_exit");

  malloc_protect_on = 0;
  ma_init();

  __memory_initialized = 1;
}

static void __memory_conclude(void) __attribute__ ((destructor));
static void __memory_conclude(void) {
  __memory_initialized = 0;

  ma_finalize();
}

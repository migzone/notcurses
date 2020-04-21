#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "internal.h"

// release the memory and fd, but don't join the thread (since we might be
// getting called within the thread's context, on a callback).
static int
ncfdplane_destroy_inner(ncfdplane* n){
  int ret = close(n->fd);
  free(n);
  return ret;
}

static void *
ncfdplane_thread(void* vncfp){
  ncfdplane* ncfp = vncfp;
  char* buf = malloc(BUFSIZ);
  ssize_t r;
  while((r = read(ncfp->fd, buf, BUFSIZ)) >= 0){
    if(r == 0){
      break;
    }
    if( (r = ncfp->cb(ncfp, buf, r, ncfp->curry)) ){
      break;
    }
  }
  // FIXME need to continue reading on pipe/socket
  if(r <= 0){
    ncfp->donecb(ncfp, r == 0 ? 0 : errno, ncfp->curry);
  }
  free(buf);
  if(ncfp->destroyed){
    ncfdplane_destroy_inner(ncfp);
  }
  return NULL;
}

ncfdplane* ncfdplane_create(ncplane* n, const ncfdplane_options* opts, int fd,
                            ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  if(fd < 0 || !cbfxn || !donecbfxn){
    return NULL;
  }
  ncfdplane* ret = malloc(sizeof(*ret));
  if(ret){
    ret->cb = cbfxn;
    ret->donecb = donecbfxn;
    ret->follow = opts->follow;
    ret->ncp = n;
    ret->destroyed = false;
    ncplane_set_scrolling(ret->ncp, true);
    ret->fd = fd;
    ret->curry = opts->curry;
    if(pthread_create(&ret->tid, NULL, ncfdplane_thread, ret)){
      free(ret);
      return NULL;
    }
  }
  return ret;
}

ncplane* ncfdplane_plane(ncfdplane* n){
  return n->ncp;
}

int ncfdplane_destroy(ncfdplane* n){
  int ret = 0;
  if(n){
    if(pthread_equal(pthread_self(), n->tid)){
      n->destroyed = true; // ncfdplane_destroy_inner() is called on thread exit
    }else{
      void* vret = NULL;
      pthread_cancel(n->tid);
      ret |= pthread_join(n->tid, &vret);
      ret |= ncfdplane_destroy_inner(n);
    }
  }
  return ret;
}

ncsubproc* ncsubproc_createv(ncplane* n, const ncsubproc_options* opts,
                             const char* bin,  char* const arg[],
                             ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  if(!cbfxn || !donecbfxn){
    return NULL;
  }
  int fd = -1;
  ncsubproc* ret = malloc(sizeof(*ret));
  if(ret){
    // FIXME create ncfdplane with pipe
    ret->pid = fork();
    if(ret->pid == 0){
      execv(bin, arg);
      fprintf(stderr, "Error execv()ing %s\n", bin);
      exit(EXIT_FAILURE);
    }else if(ret->pid < 0){
      free(ret);
      return NULL;
    }
    if((ret->nfp = ncfdplane_create(n, &opts->popts, fd, cbfxn, donecbfxn)) == NULL){
      // FIXME kill process
      free(ret);
      return NULL;
    }
  }
  return ret;
}

ncsubproc* ncsubproc_createvp(ncplane* n, const ncsubproc_options* opts,
                              const char* bin,  char* const arg[],
                              ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  if(!cbfxn || !donecbfxn){
    return NULL;
  }
  int fd = -1;
  ncsubproc* ret = malloc(sizeof(*ret));
  if(ret == NULL){
    return NULL;
  }
  int fds[2];
  if(pipe2(fds, O_CLOEXEC)){
    free(ret);
    return NULL;
  }
  // FIXME move pipe to stdio fds
  ret->pid = fork();
  if(ret->pid == 0){
    execvp(bin, arg);
    fprintf(stderr, "Error execv()ing %s\n", bin);
    exit(EXIT_FAILURE);
  }else if(ret->pid < 0){
    free(ret);
    return NULL;
  }
  if((ret->nfp = ncfdplane_create(n, &opts->popts, fd, cbfxn, donecbfxn)) == NULL){
    // FIXME kill process
    free(ret);
    return NULL;
  }
  return ret;
}

static pid_t
launch_pipe_process(int* pipe){
  int pipes[2];
  if(pipe2(pipes, O_CLOEXEC)){
    return -1;
  }
  pid_t p = fork();
  if(p == 0){
    if(dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0){
      return -1;
    }
  }else{
    *pipe = pipes[0];
  }
  return p;
}

ncsubproc* ncsubproc_createvpe(ncplane* n, const ncsubproc_options* opts,
                       const char* bin,  char* const arg[], char* const env[],
                       ncfdplane_callback cbfxn, ncfdplane_done_cb donecbfxn){
  if(!cbfxn || !donecbfxn){
    return NULL;
  }
  int fd = -1;
  ncsubproc* ret = malloc(sizeof(*ret));
  if(ret){
    ret->pid = launch_pipe_process(&fd);
    if(ret->pid == 0){
      execvpe(bin, arg, env);
      fprintf(stderr, "Error execv()ing %s\n", bin);
      exit(EXIT_FAILURE);
    }else if(ret->pid < 0){
      free(ret);
      return NULL;
    }
    if((ret->nfp = ncfdplane_create(n, &opts->popts, fd, cbfxn, donecbfxn)) == NULL){
      // FIXME kill process
      free(ret);
      return NULL;
    }
  }
  return ret;
}

int ncsubproc_destroy(ncsubproc* n){
  if(n){
    free(n);
  }
  return 0;
}
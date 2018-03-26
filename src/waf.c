#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "waf.h"


#define PIPER 0
#define PIPEW 1


typedef struct _thread_args{
    waf_t *waf;
    waf_mode mode;
} thread_args;


int main_process(waf_t *waf, int pid);

void *stdin_thread(void *args);
void *stdout_thread(void *args);
void *stderr_thread(void *args);


#define close_pipe(p) { \
    close(p[0]);        \
    close(p[1]);        \
}


waf_t *create_waf(){
    waf_t *waf = (waf_t *)malloc(sizeof(struct _waf));

    if(waf == NULL){
        perror("malloc fail");
        exit(errno);
    }

    memset(waf, 0, sizeof(struct _waf));

    pthread_mutex_init(&waf->mutex, NULL);

    return waf;
}

void delete_waf(waf_t *waf){
    // chile process has exited.
    // only close main process's pipe
    close(waf->stdin_pipe[PIPEW]);
    close(waf->stdout_pipe[PIPER]);
    close(waf->stderr_pipe[PIPER]);

    pthread_mutex_destroy(&waf->mutex);

    memset(waf, 0, sizeof(struct _waf));

    free(waf);
}

int start_stream_waf(
    waf_t *waf,
    const char *cmd,
    char* const argv[],
    char* const envp[]
){
    int pid;
    int result;

    if(pipe(waf->stdin_pipe) == -1){
        perror("create pipe stdin fail");
        exit(errno);
    }
    if(pipe(waf->stdout_pipe) == -1){
        close_pipe(waf->stdin_pipe);
        perror("create pipe stdout fail");
        exit(errno);
    }
    if(pipe(waf->stderr_pipe) == -1){
        close_pipe(waf->stdin_pipe);
        close_pipe(waf->stdout_pipe);
        perror("create pipe stderr fail");
        exit(errno);
    }

    pid = fork();

    if(pid == 0){
        #ifdef _DEBUG
        printf("child pid: %d\n", getpid());
        #endif

        if( (dup2(waf->stdin_pipe[PIPER], STDIN_FILENO) == -1) ||
            (dup2(waf->stdout_pipe[PIPEW], STDOUT_FILENO) == -1) ||
            (dup2(waf->stderr_pipe[PIPEW], STDERR_FILENO) == -1))
        {
            // dup2 fail
            close_pipe(waf->stdin_pipe);
            close_pipe(waf->stdout_pipe);
            close_pipe(waf->stderr_pipe);

            exit(errno);
        }else{
            // close unused pipe
            close(waf->stdin_pipe[PIPEW]);
            close(waf->stdout_pipe[PIPER]);
            close(waf->stderr_pipe[PIPER]);

            result = execve(cmd, argv, envp);

            exit(result);
        }
    }else if(pid > 0){
        #ifdef _DEBUG
        printf("main pid: %d\n", pid);
        #endif

        return main_process(waf, pid);

    }else{
        close_pipe(waf->stdin_pipe);
        close_pipe(waf->stdout_pipe);
        close_pipe(waf->stderr_pipe);

        perror("fork new process fail");
        exit(errno);
    }
}

int main_process(waf_t *waf, int pid){
    // close unused pipe
    close(waf->stdin_pipe[PIPER]);
    close(waf->stdout_pipe[PIPEW]);
    close(waf->stderr_pipe[PIPEW]);

    // create unlimit-loop thread
    pthread_t stdin_pid;
    pthread_t stdout_pid;
    pthread_t stderr_pid;

    pthread_create(&stdin_pid, NULL, &stdin_thread, waf);
    pthread_create(&stdout_pid, NULL, &stdout_thread, waf);
    pthread_create(&stderr_pid, NULL, &stderr_thread, waf);

    // wait for program(child process) stop
    int status;
    waitpid(pid, &status, 0);

    // stop unlimit-loop thread
    if(!pthread_kill(stdin_pid, 0)){
        pthread_cancel(stdin_pid);
    }
    if(!pthread_kill(stdout_pid, 0)){
        pthread_cancel(stdout_pid);
    }
    if(!pthread_kill(stderr_pid, 0)){
        pthread_cancel(stderr_pid);
    }

    // work done, return program status
    return status;
}

void *stdin_thread(void *args){
    waf_t *waf = (waf_t *)args;

    char d;
    int index = 0;
    char buff[WAF_BUFF];
    int isexit = 0;

    while(!isexit){
        // disable cancel signal
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        
        // read data
        if(read(STDIN_FILENO, &d, 1) == 1){
            buff[index++] = d;
        }else{
            // child process has exited
            if(index == 0){
                // all data has flushed, easy exit
                break;
            }else{
                // flush all data using NoBuff
                waf->mode = NO_BUFF;
                isexit = 1;
            }
        }

        // transport data
        if( (index == WAF_BUFF) ||  /* Full Buff */
            (waf->mode == NO_BUFF) ||
            (waf->mode == LINE_BUFF && d == '\n'))
        {
            // flush buff, write into pipe
            write(waf->stdin_pipe[PIPEW], buff, index);

            // run hook
            pthread_mutex_lock(&waf->mutex);
            (waf->stdin_hook)(buff, index, waf->stdin_args);
            pthread_mutex_unlock(&waf->mutex);

            // reset index
            index = 0;
            // enable cancel, recv cancel signal
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        }
    }

    return NULL;
}

void *stdout_thread(void *args){
    waf_t *waf = (waf_t *)args;

    char d;
    int index = 0;
    char buff[WAF_BUFF];
    int isexit = 0;

    while(!isexit){
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        if(read(waf->stdout_pipe[PIPER], &d, 1) == 1){
            buff[index++] = d;
        }else{
            if(index == 0){
                break;
            }else{
                waf->mode = NO_BUFF;
                isexit = 1;
            }
        }

        if( (index == WAF_BUFF) ||
            (waf->mode == NO_BUFF) ||
            (waf->mode == LINE_BUFF && d == '\n'))
        {
            write(STDOUT_FILENO, buff, index);

            pthread_mutex_lock(&waf->mutex);
            (waf->stdout_hook)(buff, index, waf->stdin_args);
            pthread_mutex_unlock(&waf->mutex);

            index = 0;
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        }
    }

    return NULL;
}

void *stderr_thread(void *args){
    waf_t *waf = (waf_t *)args;

    char d;
    int index = 0;
    char buff[WAF_BUFF];
    int isexit = 0;

    while(!isexit){
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        if(read(waf->stderr_pipe[PIPER], &d, 1) == 1){
            buff[index++] = d;
        }else{
            if(index == 0){
                break;
            }else{
                waf->mode = NO_BUFF;
                isexit = 1;
            }
        }

        if( (index == WAF_BUFF) ||
            (waf->mode == NO_BUFF) ||
            (waf->mode == LINE_BUFF && d == '\n'))
        {
            write(STDERR_FILENO, buff, index);

            pthread_mutex_lock(&waf->mutex);
            (waf->stderr_hook)(buff, index, waf->stderr_args);
            pthread_mutex_unlock(&waf->mutex);

            index = 0;
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        }
    }

    return NULL;
}

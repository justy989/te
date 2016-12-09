#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ncurses.h>
#include <pty.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <ctype.h>
#include <pthread.h>

typedef struct{
     bool quit;
     int command_fd;
}SendUserInputData_t;

pid_t pid;

void handle_sigchld(int signal)
{
     int stat;
     pid_t p;

     if((p = waitpid(pid, &stat, WNOHANG)) < 0){
          fprintf(stderr, "Waiting for pid %d failed: %s\n", pid, strerror(errno));
          _exit(0);
     }

     if (pid != p) return;

     if(!WIFEXITED(stat) || WEXITSTATUS(stat)){
          fprintf(stderr, "child finished with error '%d'\n", stat);
          _exit(0);
     }
}

void* send_user_input_to_terminal(void* ptr)
{
     SendUserInputData_t* data = ptr;

     while(true){
          int key = getch();
          if(key == 17){
               data->quit = true;
               break;
          }

          switch(key){
          case 263: // convert backspace to actual backspace code
               key = 8;
               break;
          }

          char character = (char)(key);

          int rc = write(data->command_fd, &character, 1);
          if(rc < 0){
               fprintf(stderr, "write() from shell failed: %s\n", strerror(errno));
               break;
          }
     }

     return NULL;
}

int main(int argc, char** argv)
{
     int term_width;
     int term_height;

     WINDOW* window = initscr();
     if(!window){
          fprintf(stderr, "initscr() failed\n");
          return -1;
     }

     keypad(stdscr, TRUE);
     raw();
     cbreak();
     noecho();

     getmaxyx(window, term_height, term_width);

     int command_fd = 0;

     // init tty
     {
          int master_fd;
          int slave_fd;
          struct winsize window_size = {term_height, term_width, 0, 0};

          if(openpty(&master_fd, &slave_fd, NULL, NULL, &window_size)){
               fprintf(stderr, "openpty() failed: %s\n", strerror(errno));
               return -1;
          }

          pid = fork();

          switch(pid){
          case -1:
               fprintf(stderr, "fork() failed\n");
               return -1;
          case 0:
          {
               setsid();

               dup2(slave_fd, STDERR_FILENO);
               dup2(slave_fd, STDOUT_FILENO);
               dup2(slave_fd, STDIN_FILENO);

               if(ioctl(slave_fd, TIOCSCTTY, NULL) < 0){
                    fprintf(stderr, "ioctl() failed: %s\n", strerror(errno));
                    return -1;
               }

               close(slave_fd);
               close(master_fd);

               // exec shell
               const struct passwd *pw = getpwuid(getuid());
               if(pw == NULL){
                    fprintf(stderr, "getpwuid() failed %s\n", strerror(errno));
                    return -1;
               }

               char* sh = (pw->pw_shell[0]) ? pw->pw_shell : "/bin/bash";

               // resetting env
               unsetenv("COLUMNS");
               unsetenv("LINES");
               unsetenv("TERMCAP");
               setenv("LOGNAME", pw->pw_name, 1);
               setenv("USER", pw->pw_name, 1);
               setenv("SHELL", sh, 1);
               setenv("HOME", pw->pw_dir, 1);
               setenv("TERM", "te", 1);

               // resetting signals
               signal(SIGCHLD, SIG_DFL);
               signal(SIGHUP, SIG_DFL);
               signal(SIGINT, SIG_DFL);
               signal(SIGQUIT, SIG_DFL);
               signal(SIGTERM, SIG_DFL);
               signal(SIGALRM, SIG_DFL);

               char** args = NULL;
               execvp(sh, args);
               _exit(1);
          } break;
          default:
               close(slave_fd);
               command_fd = master_fd;

               signal(SIGCHLD, handle_sigchld);
               break;
          }
     }

     char buffer[BUFSIZ];
     char bytes_read_from_terminal[BUFSIZ];
     int buffer_end = 0;
     int key = getch();

     memset(buffer, 0, BUFSIZ);
     memset(bytes_read_from_terminal, 0, BUFSIZ);

     SendUserInputData_t send_user_input_data = {false, command_fd};
     pthread_t user_input_thread;

     int rc = pthread_create(&user_input_thread, NULL, send_user_input_to_terminal, &send_user_input_data);
     if(rc != 0){
          printf("pthread_create() failed\n");
          endwin();
          return -1;
     }

     while(!send_user_input_data.quit){
          rc = read(command_fd, bytes_read_from_terminal, BUFSIZ);
          if(rc < 0){
               fprintf(stderr, "read() from shell failed: %s\n", strerror(errno));
               endwin();
               return -1;
          }

          bytes_read_from_terminal[rc] = 0;

          char* read_byte = bytes_read_from_terminal;
          while(*read_byte){
               if(isprint(*read_byte)){
                    buffer[buffer_end] = *read_byte;
                    buffer_end++;
               }else{
                    switch(*read_byte){
                    case 0x08: // backspace
                         buffer_end--;
                         buffer[buffer_end] = 0;
                         if(buffer_end < 0) buffer_end = 0;
                         break;
                    case '\n':
                         buffer[buffer_end] = *read_byte;
                         buffer_end++;
                         break;
                    default:
                         break;
                    }
               }

               read_byte++;
          }

          erase();
          mvprintw(0, 0, "[te] %d %s", key, buffer);
          refresh();
     }

     pthread_join(user_input_thread, NULL);

     endwin();
     return 0;
}

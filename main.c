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

     char output[BUFSIZ];
     int last_char = 0;
     int key = getch();

     memset(output, 0, BUFSIZ);

     int rc = read(command_fd, output, BUFSIZ);
     if(rc < 0){
          endwin();
          fprintf(stderr, "read() from shell failed: %s\n", strerror(errno));
          return -1;
     }

     last_char += rc;
     output[last_char] = 0;

     erase();
     printw("[te] %d %s", key, output);
     refresh();

     while(true){
          key = getch();
          if(key == 17) break;
          char character = (char)(key);

          rc = write(command_fd, &character, 1);
          if(rc < 0){
               fprintf(stderr, "write() from shell failed: %s\n", strerror(errno));
               endwin();
               return -1;
          }

          int rc = read(command_fd, output + last_char, BUFSIZ - last_char);
          if(rc < 0){
               fprintf(stderr, "read() from shell failed: %s\n", strerror(errno));
               endwin();
               return -1;
          }

          char* output_before_update = output + last_char;

          last_char += rc;
          output[last_char] = 0;

          // ncurses doesn't like the '\r'
          for(int i = 0; i < rc; i++){
               if(output_before_update[i] == '\r') output_before_update[i] = ' ';
          }

          erase();
          mvprintw(0, 0, "[te] %d %s", key, output);
          refresh();
     }

     endwin();
     return 0;
}

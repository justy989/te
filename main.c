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

#define COLOR_BACKGROUND -1
#define COLOR_FOREGROUND -1

typedef struct{
     int x;
     int y;
}Point_t;

typedef struct{
     bool quit;
     int command_fd;
}SendUserInputData_t;

typedef struct{
     int id;
     int fg;
     int bg;
}Color_t;

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

void color_change_foreground(Color_t* color, int fg)
{
     attroff(COLOR_PAIR(color->id));

     color->id++;
     color->fg = fg;
     init_pair(color->id, color->fg, color->bg);

     attron(COLOR_PAIR(color->id));
}

void color_change_background(Color_t* color, int bg)
{
     attroff(COLOR_PAIR(color->id));

     color->id++;
     color->bg = bg;
     init_pair(color->id, color->fg, color->bg);

     attron(COLOR_PAIR(color->id));
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
     scrollok(stdscr, TRUE);

     if(has_colors() != TRUE){
          fprintf(stderr, "%s doesn't support colors\n", getenv("TERM"));
          return -1;
     }

     start_color();
     use_default_colors();

     getmaxyx(window, term_height, term_width);

     Color_t color;
     color.id = 0;
     color.fg = COLOR_FOREGROUND;
     color.bg = COLOR_BACKGROUND;

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
               setenv("TERM", "dumb", 1);

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

     char bytes_read_from_terminal[BUFSIZ];
     getch();
     Point_t cursor = {0, 0};
     Point_t save_cursor = {0, 0};

     memset(bytes_read_from_terminal, 0, BUFSIZ);

     SendUserInputData_t send_user_input_data = {false, command_fd};
     pthread_t user_input_thread;

     int rc = pthread_create(&user_input_thread, NULL, send_user_input_to_terminal, &send_user_input_data);
     if(rc != 0){
          printf("pthread_create() failed\n");
          endwin();
          return -1;
     }

     bool escape = false;
     bool csi = false;
     int csi_arguments[16]; // NPAR, does it exist?
     int csi_argument_index = 0;
     char digit = '0';
     char prev_digit = '0';

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
               if(escape){
                    switch(*read_byte){
                    default:
                         break;
                    case 'E':
                         cursor.x = 0;
                         cursor.y++;
                         break;
                    case '[':
                         csi = true;
                         break;
                    }

                    escape = false;
               }else if(csi){
                    if(isdigit(*read_byte)){
                         prev_digit = digit;
                         digit = *read_byte;
                    }else{
                         // NOTE: only needed for a subset of the options
                         char number_string[3] = {prev_digit, digit, 0};
                         csi_arguments[csi_argument_index] = atoi(number_string);

                         if(*read_byte == ';'){
                              // another argument incoming
                              csi_argument_index++;
                              prev_digit = '0';
                              digit = '0';
                         }else{
                              switch(*read_byte){
                              default:
                                   abort();
                                   break;
                              case '@':
                                   for(int i = 0; i < csi_arguments[0]; ++i){
                                        addch(' ');
                                   }
                                   break;
                              case 'A':
                                   cursor.y -= csi_arguments[0];
                                   break;
                              case 'B':
                                   cursor.y += csi_arguments[0];
                                   break;
                              case 'C':
                                   cursor.x += csi_arguments[0];
                                   break;
                              case 'D':
                                   cursor.x -= csi_arguments[0];
                                   break;
                              case 'E':
                                   cursor.y += csi_arguments[0];
                                   cursor.x = 0;
                                   break;
                              case 'F':
                                   cursor.y -= csi_arguments[0];
                                   cursor.x = 0;
                                   break;
                              case 'G':
                                   cursor.x = csi_arguments[0] - 1;
                                   if(cursor.x < 0) cursor.x = 0;
                                   break;
                              case 'H':
                                   cursor.y = csi_arguments[0] - 1;
                                   cursor.x = csi_arguments[1] - 1;

                                   if(cursor.y < 0) cursor.y = 0;
                                   if(cursor.x < 0) cursor.x = 0;
                                   break;
                              case 'J':
                                   switch(csi_arguments[0]){
                                   default:
                                        clrtobot();
                                        break;
                                   case 1:
                                   {
                                        int count = (term_width * cursor.y) + cursor.x;

                                        move(0, 0);
                                        for(int i = 0; i < count; ++i){
                                             addch(' ');
                                        }
                                   } break;
                                   case 2:
                                   case 3:
                                        erase();
                                        break;
                                   }
                                   break;
                              case 'K':
                                   switch(csi_arguments[0]){
                                   default:
                                        clrtoeol();
                                        break;
                                   case 1:
                                        move(cursor.y, 0);
                                        for(int i = 0; i < cursor.x; ++i){
                                             addch(' ');
                                        }
                                        break;
                                   case 2:
                                        move(cursor.y, 0);
                                        clrtoeol();
                                        break;
                                   }
                                   break;
                              //case 'L': // TODO: insert csi_arguments blank lines
                              //     break;
                              case 'M':
                                   for(int i = 0; i < csi_arguments[0]; ++i){
                                        move(cursor.y + i, 0);
                                        clrtoeol();
                                   }
                                   break;
                              //case 'P': // TODO: delete csi_arguments characters
                              //     break;
                              //case 'X': // TODO: erase csi_arguments characters (how is erase different from delete?)
                              //     break;
                              case 'a':
                                   cursor.x += csi_arguments[0];
                                   break;
                              case 'c': // answer "I am a VT102"
                                   break;
                              case 'd':
                                   cursor.y = csi_arguments[0] - 1;
                                   if(cursor.y < 0) cursor.y = 0;
                                   break;
                              case 'e':
                                   cursor.y += csi_arguments[0];
                                   break;
                              case 'f':
                                   cursor.y = csi_arguments[0] - 1;
                                   cursor.x = csi_arguments[1] - 1;
                                   if(cursor.x < 0) cursor.x = 0;
                                   if(cursor.y < 0) cursor.y = 0;
                                   break;
                              case 'g':
                                   break;
                              case 's':
                                   save_cursor = cursor;
                                   break;
                              case 'u':
                                   cursor = save_cursor;
                                   break;
                              case 'm':
                                   standend();

                                   // set color
                                   switch(csi_arguments[0]){
                                   default:
                                        break;
                                   case 0:
                                        standend();
                                        color.fg = COLOR_FOREGROUND;
                                        color.bg = COLOR_BACKGROUND;
                                        break;
                                   case 1:
                                        attron(A_BOLD);
                                        break;
                                   case 7:
                                        attron(A_REVERSE);
                                        break;
                                   case 27:
                                        attroff(A_REVERSE);
                                        break;
                                   case 30:
                                        color_change_foreground(&color, COLOR_BLACK);
                                        break;
                                   case 31:
                                        color_change_foreground(&color, COLOR_RED);
                                        break;
                                   case 32:
                                        color_change_foreground(&color, COLOR_GREEN);
                                        break;
                                   case 33:
                                        color_change_foreground(&color, COLOR_YELLOW);
                                        break;
                                   case 34:
                                        color_change_foreground(&color, COLOR_BLUE);
                                        break;
                                   case 35:
                                        color_change_foreground(&color, COLOR_MAGENTA);
                                        break;
                                   case 36:
                                        color_change_foreground(&color, COLOR_CYAN);
                                        break;
                                   case 37:
                                        color_change_foreground(&color, COLOR_WHITE);
                                        break;
                                   case 38: // TODO: underscore on
                                        color_change_foreground(&color, COLOR_FOREGROUND);
                                        break;
                                   case 39: // TODO: underscore off
                                        color_change_foreground(&color, COLOR_FOREGROUND);
                                        break;
                                   case 40:
                                        color_change_background(&color, COLOR_BLACK);
                                        break;
                                   case 41:
                                        color_change_background(&color, COLOR_RED);
                                        break;
                                   case 42:
                                        color_change_background(&color, COLOR_GREEN);
                                        break;
                                   case 43:
                                        color_change_background(&color, COLOR_YELLOW);
                                        break;
                                   case 44:
                                        color_change_background(&color, COLOR_BLUE);
                                        break;
                                   case 45:
                                        color_change_background(&color, COLOR_MAGENTA);
                                        break;
                                   case 46:
                                        color_change_background(&color, COLOR_CYAN);
                                        break;
                                   case 47:
                                        color_change_background(&color, COLOR_WHITE);
                                        break;
                                   case 49:
                                        color_change_background(&color, COLOR_BACKGROUND);
                                        break;
                                   }

                                   prev_digit = '0';
                                   digit = '0';
                                   break;
                              }

                              csi = false;
                              csi_argument_index = 0;
                         }
                    }
               }else if(isprint(*read_byte)){
                    mvprintw(cursor.y, cursor.x, "%c", *read_byte);
                    cursor.x++;
               }else{
                    switch(*read_byte){
                    default:
                         break;
                    case 27: // escape: "Control Sequnce Introducer"
                         escape = true;
                         break;
                    case 8: // backspace
                         mvprintw(cursor.y, cursor.x, " ", *read_byte);
                         cursor.x--;
                         break;
                    case '\n':
                         cursor.x = 0;
                         cursor.y++;
                         if(cursor.y >= term_height){
                              scrl(1);
                              cursor.y = term_height - 1;
                         }
                         break;
                    }
               }

               read_byte++;
          }

          move(cursor.y, cursor.x);
          refresh();
     }

     pthread_join(user_input_thread, NULL);

     endwin();
     return 0;
}

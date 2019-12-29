#include <ctype.h>
#include <errno.h>
#include <fcntl.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

#define MAX_DIR_SIZE 100

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

/* Process id for the child process */
pid_t child_pid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_wait(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_cd, "cd", "changes the current working directory to the argument taken"},
  {cmd_pwd, "pwd", "prints the current working directory to standard output"},
  {cmd_wait, "wait", "waits until all background jobs have terminated before returning to the prompt"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* Changes the current directory to the argument taken */
int cmd_cd(struct tokens *tokens) {
	if (chdir(tokens_get_token(tokens, 1)) != 0) {
		printf("No such directory\n");
	}
	return 1;
}

/* Prints the current working directory to standard output */
int cmd_pwd(unused struct tokens *tokens) {
	char path[MAX_DIR_SIZE];
	getcwd(path, MAX_DIR_SIZE);
	printf("%s\n", path);
	return 1;
}

/* Waits until all background jobs have terminated before returning to the prompt */
int cmd_wait(struct tokens *UNUSED) {
	int status;
	pid_t pid;
    while ((pid = wait(&status))){
      if (pid == -1) {
      	break;
	  }
	}
	return 0;
}

/* Resolve program names from the environment */
char* resolve(char *name) {
	char *path = getenv("PATH");
	char *path_seg;
	char *path2 = (char*) malloc(strlen(path) + 1);
	strcpy(path2, path);
	path_seg = strtok(path2, ":");
	char *path_seg2 = (char*) malloc(strlen(path_seg) + 1);
	strcpy(path_seg2, path_seg);
	strcat(path_seg2, "/");
	strcat(path_seg2, name);
	if (access(path_seg2, F_OK) == 0) {
		free(path2);
		return path_seg2;
	}
	while ((path_seg = strtok(NULL, ":"))) {
		char *path_seg2 = (char*) malloc(strlen(path_seg) + 1);
		strcpy(path_seg2, path_seg);
		strcat(path_seg2, "/");
		strcat(path_seg2, name);
		if (access(path_seg2, F_OK) == 0) {
			free(path2);
			return path_seg2;
		}
	}
	free(path2);
	free(path_seg2); 
	return NULL;
}

/* Excute the program */
int excute(struct tokens *tokens) {
	int length = tokens_get_length(tokens);
	int i;
	int j = 0;
	char *infile, *outfile;
	char** argv = (char**) malloc(length*sizeof(char*));
	for (i = 0; i < length; i++) {
		int fd;
		if (*tokens_get_token(tokens, i) != '>' && *tokens_get_token(tokens, i) != '<') {
			argv[j] = tokens_get_token(tokens, i);
			j += 1;		
		} else if (*tokens_get_token(tokens, i) == '<') {
			infile = tokens_get_token(tokens, i+1);
			fd = open(infile, O_RDONLY);
			dup2(fd, STDIN_FILENO);
			i += 1; 
		} else {
			outfile = tokens_get_token(tokens, i+1);
			fd = open(outfile, O_WRONLY | O_CREAT, S_IRWXU);
			dup2(fd, STDOUT_FILENO);
			i += 1;	
		}
	}
	argv[j] = NULL;
	if (strchr(argv[0],'/') == NULL) {
	 	argv[0] = resolve(argv[0]);
	}
	int status = execv(argv[0], argv);
	free(argv);
	return status;
}

/* Handler that kill the child process */
void handler(int signum) {
	kill(child_pid, SIGINT);
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;
  
  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */
      int background = 0;
      if (*tokens_get_token(tokens, tokens_get_length(tokens)-1) == '&') {
      	background = 1;
	  }
      pid_t pid = fork();
      int status;
	  if (pid == 0) { 

	  	if (excute(tokens) == -1) {
	  	  	fprintf(stdout, "This shell doesn't know how to run programs.\n");		
		}	  	
	  } else if (pid > 0) {	  	
	  	if (background == 0) {
	  	  setpgid(pid, pid);
	  	  child_pid = pid;
	  	  struct sigaction act;
	  	  act.sa_handler = handler;

	  	  signal(SIGTTOU, SIG_IGN);
	  	  sigaction(SIGINT, &act, NULL);
	      waitpid(pid, &status, 0); 	  
	  	}
	  }
       	    
    }
    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}

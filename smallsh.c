#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stddef.h>

#define SIZE 5

struct command_input {
  char *command;
  char *arguments[512];
  char *input_file;
  char *output_file;
  bool background;
 };

struct bg_process {
    int pid;
    int exit_status;
    int run_status;
};

enum run_status {Incomplete, Complete, Printed_Complete, Never_Run};

int fg_mode;

// Parsing //
void command_parser(char *, struct command_input *, int);
char *variable_expansion (int, char *);

// Exec Funtion //
void fg_command_exec(struct command_input *, int, char *);
void bg_command_exec(struct command_input *, struct bg_process *);

// Redirection //
void input_redirection(char *, struct command_input *);
void output_redirection(char *, struct command_input *);

// Build-in Functions //
void bi_exit(struct bg_process *);
void bi_cd(struct command_input *);
void bi_status(char *);

// Background Functions //
void bg_array_initiator(struct bg_process *);
void bg_printer(struct bg_process *);
void bg_checker(struct bg_process *);
void bg_array_insert(struct bg_process *, int);

// Signals //
void signal_handler(int, void(*)(int));
void enter_fg(int);
void exit_fg(int);

// Support Functions //
void reset_ci(struct command_input *);



////////// Main //////////
int main(void) {

  // Create empty structure to hold command line input
  struct command_input ci;
  memset(&ci, 0, sizeof(struct command_input));  
  
  // Establish shell PID and default status and status_message
  int pid = getpid();
  int status = 0;
  char status_message[50];

  // Initizalize an array of bg_process structrs to hold background process information
  // Run funtion to ensure structs in array are empty
  struct bg_process bg_array[SIZE];
  bg_array_initiator(bg_array);
  
  // Set parent to ignore SIGINT signal
  signal_handler(SIGINT, SIG_IGN);

  // Set parent to enter foregound mode upon receiving signal
  signal_handler(SIGTSTP, enter_fg);

  // Allocate memory for inpit
  char input[2049];
  
  // Start loop to run shell until exit command received
  while(true) {
   
    // Checks if any background processes have terminated
    // Prints the pid and status of any background processes that have terminated
    bg_checker(bg_array);
    bg_printer(bg_array);
    
    // Prompts user
    printf(": ");
    fflush(stdout);

    // Unblock the SIGTSTP signal before receiving input  
    sigset_t new_mask;
    sigemptyset(&new_mask);
    sigaddset(&new_mask, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &new_mask, NULL);

    // Ensure input memory is clear before receiving input
    memset(&input, 0, sizeof(input));

    // Receive command, store in input
    // If there is an error, restart loop and reprompt
    if (fgets((char *)&input, sizeof(input), stdin) == NULL) {  
      clearerr(stdin);
      continue;
    }
    
    // Reblock to the SIGTSTP signal
    sigprocmask(SIG_BLOCK, &new_mask, NULL);

    // If comment or blank line clear input and reprompt
    if(input[0] == '#' || strcmp(input, "\n") == 0 || strcmp(input,"") == 0) {
        continue;
    }
    
    // Parse input into structure, clear input and exit prompt loop
    command_parser(input, &ci, pid);
  
    // Check for built-in "exit" function
    if(strcmp(ci.command, "exit") == 0) {
      bi_exit(bg_array);
      return 0;
    }

    // Check for built-in "cd" function
    else if(strcmp(ci.command, "cd") == 0) {
      bi_cd(&ci);
    }
  
    // Check for built-in "status" function
    else if(strcmp(ci.command, "status") == 0) {
      bi_status(status_message);
    }

    // Executes command in foreground
    else if(ci.background == 0 || fg_mode == 1) {
      fg_command_exec(&ci, status, status_message);
    }

    else if(ci.background == 1 && fg_mode == 0) {
      bg_command_exec(&ci, bg_array);
    }

    // Clears the memory of the command input stucture for next prompt
    reset_ci(&ci);
  }
}



////////// Parses the command into a structure //////////
void command_parser(char *input, struct command_input *ci, int pid) {

  // Creates pointer to first word in input sting
  char *segment = strtok(input, " \n");
  
  // Expansion returns new pointer, segment does not point at input 
  // Copy segment into command input structure
  // Segment memory is no longer needed
  if (segment != NULL) {  
    segment = variable_expansion(pid, segment);
    ci->command = strdup(segment);
    ci->arguments[0] = strdup(segment);
    free(segment);
  }

  // Reassign pointer to second word in input string
  segment = strtok(NULL, " \n");

  // While loop fills argument array
  // Loop continues until segment is null (end of string)
  // Or end of argument character is reached
  int arg_index = 1;
  while (segment != NULL && !((strcmp(segment, ">") == 0 || strcmp(segment, "<") == 0) || strcmp(segment, "&") == 0)) {

    // Segment reassigned to new pointer
    // Segment is copied into command input struct and memory is freed
    segment = variable_expansion(pid, segment);
    ci->arguments[arg_index] = strdup(segment);
    free(segment);
    
    // Reassign pointer to next word in input
    segment = strtok(NULL, " \n");

    arg_index++;
  }

  // After agrument array is filled, while loop parses input, output and background flags
  while (segment != NULL) {
     
    segment = variable_expansion(pid, segment);
    
    // Create a new pointer to next word in input
    char *next_seg = strtok(NULL, " \n");

    // Expansion returns new pointer, segment does not point at input 
    // Copy the next word after segment into structure
    // Next segment memory no longer needed
    if(next_seg != NULL) {
       next_seg = variable_expansion(pid, next_seg);  
      if(strcmp(segment, "<") == 0) {
        ci->input_file = strdup(next_seg);
      }
      if(strcmp(segment, ">") == 0) {
        ci->output_file = strdup(next_seg);
      }
      free(next_seg);
    }

    // Assigns bool to structure
    // Segment memory is no longer needed
    if(strcmp(segment, "&") == 0 && next_seg == NULL) {
      ci->background = 1;
    }
    free(segment);

    // Reassign pointer to next world after next
    segment = strtok(NULL, " \n");
  }

}



////////// Function which replaces "$$" string with shell pid //////////
char * variable_expansion (int pid, char *segment) {

  // Convert pid to string
  char spid[12];
  sprintf(spid, "%d", pid); 

  // Establish orginal sizes
  int pid_length = strlen(spid);
  int seg_length = strlen(segment);

  // Create new segment that is worst case size (all $$)
  // Plus one for the null terminator
  char *new_segment = malloc(seg_length * (pid_length - 2) + 1);

  int new_index = 0;

  // foo$$
  // 000000000000000
  // f00000000000000
  // foo000000000000
  // foo123450000000
  //
  // $$$$$$
  // 000000000000000
  // 123450000000000
  // 123451234500000
  // 123451234512345
  //
  // $$foo
  // 0000000000000000
  // 1234500000000000
  // 12345f0000000000
  // 12345fo000000000
  // 12345foo00000000

  // Loop through orginal segement
  for(int orig_index = 0; orig_index < seg_length; ) {
    
    // Returns a pointer at location of first occurance of substring
    // Returns NULL if no substring is found
    char *match_ptr = strstr(&segment[orig_index], "$$");
    
    // If the match pointer is at the same location as the current orig_index, match_bool = True
    bool match_bool = (match_ptr == &segment[orig_index]);

    // If match_bool is true, copy string pid at location of new_index then move both indexes forward
    // If match_bool is false, copy the value at the old index to the new segment at new index and move both indexes forward
    if(match_bool) {
      strcpy(&new_segment[new_index], spid);
      orig_index += 2;
      new_index += pid_length;
    } else {
     new_segment[new_index] = segment[orig_index];
     orig_index += 1;
     new_index += 1;
    }
  }

  // Add null terminator to new segment
  new_segment[new_index] = '\0';
  
  return new_segment;
};



//////////Executes commands in the foreground //////////
void fg_command_exec(struct command_input *ci, int status, char *status_message) {  

  int childStatus;
  pid_t childPid = fork();

	switch(childPid){
	  case -1:
		  perror("fork()\n");
		  exit(1);

    case 0:
      // If input_file or outpur file exists, redirect
      if(ci->input_file != NULL) {
        input_redirection(ci->input_file, ci);
      }
        
      if(ci->output_file != NULL) {
        output_redirection(ci->output_file, ci);
      }

      // Set child processes to use default for SIGINT
      signal_handler(SIGINT,SIG_DFL);

      // Set child processes ignore signal SIGTSTP
      signal_handler(SIGTSTP, SIG_IGN);
        
      // Excute command and print error is execution fails
      execvp(ci->command, ci->arguments);
		  perror("execve");
		  exit(2);

	  default:
		  childPid = waitpid(childPid, &childStatus, 0);
          
      if(WIFEXITED(childStatus)) {
        status = WEXITSTATUS(childStatus);
        sprintf(status_message, "exited with %d\n", status);
      }
      else if(WIFSIGNALED(childStatus)) {
        status = WTERMSIG(childStatus);
        sprintf(status_message, "terminated by signal %d\n", status);
        bi_status(status_message);
      }
   } 
};


////////// Executes commands in the background //////////
void bg_command_exec(struct command_input *ci, struct bg_process *bg_array) {  
  int childStatus;
  pid_t childPid = fork();

	switch(childPid){
	  case -1:
		  perror("fork()\n");
		  exit(1);

	  case 0:
      // If no input or output file is specified
      // Redirect to "/dev/null"
      if(ci->input_file == NULL) {
        input_redirection("/dev/null", ci);
      }
      
      if(ci->output_file == NULL) { 
        output_redirection("/dev/null", ci);
      }

      // Set background process to ignore signal SIGTSTP
      signal_handler(SIGTSTP, SIG_IGN);
        
      // Excute command and print error is execution fails
      execvp(ci->command, ci->arguments);
		  perror("execve");
		  exit(2);

	  default:
      // Checks if any backround process have finished
      // If so, updates terminated flag
      // First check the background array for free space
      // Then add the childPID to the background array
      bg_checker(bg_array);
      printf("background pid is %d\n", childPid);
      fflush(stdout);
      bg_array_insert(bg_array, childPid);
  }
};



////////// Redirects input from stdin to pointer at *path //////////
void input_redirection(char *path, struct command_input *ci) {
  int file_desc = open(path, O_RDONLY);
  if(file_desc == -1) {
    perror("read file open failed");
    exit(1);
  }
  int file_redirect = dup2(file_desc, STDIN_FILENO);
  if(file_redirect == -1) {
    perror("read file redirect failed");
    exit(2);
    close(file_desc);
  }
};



////////// Redirects output from stdout to pointer at *path //////////
void output_redirection(char *path, struct command_input *ci) {
  int file_desc = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
  if(file_desc == -1) {
    perror("write file open failed");
    exit(1);
  }
  int file_redirect = dup2(file_desc, STDOUT_FILENO);
  if(file_redirect == -1) {
    perror("write file redirect failed");
    exit(2);
    close(file_desc);
  }
};



////////// Cleans up all currenly running backround processes //////////
void bi_exit(struct bg_process *bg_array) {
  for(int i = 0; i < SIZE;  i++) {
    if(bg_array[i].pid != 0) {
      kill(bg_array[i].pid, SIGKILL);
    }
  }
};



////////// Changes working directory //////////
void bi_cd(struct command_input *ci) {
  if(ci->arguments[1] != NULL) {
    chdir(ci->arguments[1]);
  } else {
    chdir(getenv("HOME"));
  }
};



////////// Prints last foreground process' termination status //////////
void bi_status(char *status_message) {
  printf("%s", status_message);
  fflush(stdout);
};



////////// Loops through array of background processes and fills with empty background process structures //////////
void bg_array_initiator(struct bg_process *bg_array) {
  for(int i = 0; i < SIZE;  i++) {
    struct bg_process temp = {.pid = 0, .exit_status = 0, .run_status = Never_Run };
    bg_array[i] = temp;
  }
};



////////// Loops through array of background processes, prints message and updates flag if process has terminated //////////
void bg_printer(struct bg_process *bg_array) {
  for(int i = 0; i < SIZE; i++) {
    if(bg_array[i].run_status == Complete) {
        printf("background pid %d is done: exit value %d\n", bg_array[i].pid, bg_array[i].exit_status);
        fflush(stdout);
        bg_array[i].run_status = Printed_Complete;
    }
  }
};



////////// Loops through array of background processes, checks if processes have finished, if so updates run_status flag and exit status //////////
void bg_checker(struct bg_process *bg_array) {
  for (int i = 0; i < SIZE; i++) {
    if(bg_array[i].pid == 0 || bg_array[i].run_status == Printed_Complete) {
      continue;
    }
    if(waitpid(bg_array[i].pid, &bg_array[i].exit_status, WNOHANG) > 0) {
      bg_array[i].run_status = Complete;
    }
  }
};



////////// Loops through array of background processes and searchs for "empty" array spot //////////
////////// Empty processes are those that have never been run or run and message has been printed to stdout //////////
////////// Prints error if there are no "empty spots //////////
void bg_array_insert(struct bg_process *bg_array, int pid) {
  int empty_count = 5; 
  struct bg_process temp = {.pid = pid, .exit_status = 0, .run_status = Incomplete };
  for(int i = 0; i < SIZE;  i++){
    if(bg_array[i].run_status == Printed_Complete || bg_array[i].run_status == Never_Run) {
      bg_array[i] = temp;
      return;
    } else {
      empty_count --;
    }
  }
  if (empty_count <= 0) {
    printf("Error: background array is full\n");
    fflush(stdout);
  }
};



////////// Applies the action of the function from handler_func to the signal provided by signo //////////
void signal_handler(int signo, void(*handler_func)(int)) {

	struct sigaction action = {0};
	action.sa_handler = handler_func;
	sigfillset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(signo, &action, NULL);
};



////////// Enters foreground mode, updates signal hander and foreground mode bool //////////
void enter_fg(int signo) {
  signal_handler(signo, exit_fg);

  char *message = "\nEntering foreground-only mode (& is now ignored)\n";
  write(STDOUT_FILENO, message, 50);
  
  fg_mode = 1;
};



////////// Enters foreground mode, updates signal hander and foreground mode bool //////////
void exit_fg(int signo) {
  signal_handler(signo, enter_fg);
  
  char *message = "\nExiting foreground-only mode\n";
  write(STDOUT_FILENO, message, 33);
  
  fg_mode = 0;
};



////////// Clears memory used by the command input structure and resets to empty //////////
void reset_ci(struct command_input *ci) {
  if(ci->command != NULL) {
    free(ci->command);
  }
  
  if(ci->input_file != NULL) {
    free(ci->input_file);
  }
  
  if(ci->output_file != NULL) {
    free(ci->output_file);
  }
 
  for(int i = 0; i < 512; i++) {
    if(ci->arguments[i] != NULL) {
      free(ci->arguments[i]);
    }
  }

  memset(ci, 0, sizeof(struct command_input));  
};


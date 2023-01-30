#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <err.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define _XOPEN_SOURCE 700 // used for sigaction. Suggested as per https://stackoverflow.com/questions/6491019/struct-sigaction-incomplete-error

struct commandLine {
  char* argsList[512]; //can have at most 512 arguments
  char* inputFile;
  char* outputFile;
  bool backgroundOrForeground; //used to determine if background or foreground. If the command is to be executed in the background, the last word must be '&'
  bool inputCmd; // true if there is input redirection, false if there is not
  bool outputCmd; //true if there is output redirection, false if there is not
  int status;  // store the last status given by the child status
  int* totalBgProcess; // store the total number of background processes, will be needed for background process array

};

//global variables to be used for tracking background pids
int pidIncrement = 0;
int pidList[128];

// global to be used to track foreground mode vs not
volatile sig_atomic_t sigFlag;

// function to handle CTRL-Z signal
void handle_SIGTSTP(int signo){
  if(sigFlag == 1){
    char* prompt= "Exiting foreground-only mode\n";
    write(STDOUT_FILENO, prompt, strlen(prompt));
  } 
  // foreground mode
  else {
    char* prompt= "\nEntering foreground-only mode (& is now ignored)\n";
    write(STDOUT_FILENO, prompt, strlen(prompt));
  }
  sigFlag = 1 - sigFlag; // switch the flag with each CTRL-Z signal
}

/**
 * @brief This function changes the directory. If a user enters just cd, it changes to the directory of the HOME environment variable, otherwise it redirecrs to the path
 * specified.
 * 
 * @param commandLineStruct 
 * @param numOfArgs 
 */
void CD(struct commandLine* commandLineStruct, int numOfArgs){
  if(numOfArgs == 1){
    chdir(getenv("HOME"));
  } else {
    chdir(commandLineStruct->argsList[1]);
  }
}

/**
 * @brief This function provides the last status as provided by the wstatus indicator
 * 
 * @param commandLineStruct 
 */
void updateStatus(struct commandLine* commandLineStruct){
  // code implemented from Canvas Exploration: Process API - Monitoring Child Processes
  if(WIFEXITED(commandLineStruct->status)){
    printf("exit value %d\n", WEXITSTATUS(commandLineStruct->status));
  } else if(WIFSIGNALED(commandLineStruct->status)){
    printf("terminated by signal %d\n", WTERMSIG(commandLineStruct->status));
  }
}


/**
 * @brief This function kills any other processes or jobs started and then exits the shell
 * 
 */
void exitShell(){
  // kill all the remaining background pids
  int i = 0;
  for(i; i < pidIncrement; i++) {
    kill(pidList[i], SIGKILL);
  }
  // exit the shell
  exit(EXIT_SUCCESS);
}


/**
 * @brief This function reaps a zombie child process that was run in the background and provides data on it such as the PID, status, and if it was killed 
 * by a signal.
 * 
 * @param commandLineStruct 
 * 
 */
void checkBackgroundProcess(struct commandLine* commandLineStruct){
  //worked with OSU tutor on proceeding lines 105 to 119            
  int i=0;
  int j=0;
  int childStatus;
  for (i; i < pidIncrement; i++){
    if(waitpid(pidList[i], &childStatus , WNOHANG)){ //check if the pid is done
      if (sigFlag == 0){ // if not in foreground only mode
        // modeled after Canvas Exploration: Process API - Monitoring Child Processes
        if(WIFEXITED(childStatus)){ // if child process ended normally
          printf("background pid %d is done: exit value %d\n",  pidList[i], WEXITSTATUS(childStatus));
        } else if(WIFSIGNALED(childStatus)){ // else if the child process was terminated abnormally
          printf("background pid %d is done: terminated by signal %d\n", pidList[i], WTERMSIG(childStatus)); // return the signal number that caused the child to terminate
        }
      }
      for(j; j < pidIncrement - 1; j++) {
        pidList[j] = pidList[j + 1]; // move the next argument into pid 
      }
      pidIncrement--; // decrement pid as the total number of processes to check have decreased
    }
  }
}


/**
 * @brief This funciton prompts the user to enter input and then parses that input into the appropriate member of the struct. If a user enters a comment or 
 * a blank line, the user is immediately reprompted. This procedure also checks the status of completed background processes before returning access to the command line to the user
 * 
 * @param userInput 
 * @param commandLineStruct 
 * @param numOfArgs 
 */
void commandLineAndInput(char* userInput, struct commandLine* commandLineStruct, int* numOfArgs, int sigFlag){
  
  checkBackgroundProcess(commandLineStruct); //check if any background processes terminated
  

  //prompt the user and immediately flush the output buffer to display the command prompt
  printf(":");
  fflush(stdout);

  // clear out the args list each time before receiving args from the user
  int i = 0;
  for(i; i < 512; i++){
    commandLineStruct->argsList[i] = NULL;
  }
  // initialize the arguments to foreground each time
  commandLineStruct->backgroundOrForeground = false;

  // initialize file I/O redirection to false every time
  commandLineStruct->inputCmd = false;
  commandLineStruct->outputCmd = false;

  //clear the file contents
  commandLineStruct->inputFile = NULL;
  commandLineStruct->outputFile = NULL;

  userInput = malloc(2048 * sizeof(char));                                            // need to remember to free this!!!!!!!
  size_t buffer= 2048; // max of 2048 characters
  getline(&userInput, &buffer, stdin);
  userInput[strlen(userInput)-1] = '\0'; //remove the newline before tokenizing the string
  // if no input, or input is comment return the prompt
  if(strlen(userInput) == 0 || strstr(userInput, "#")){
    commandLineAndInput(userInput, commandLineStruct, numOfArgs, sigFlag);
  }else {

    //split the user input by spaces. Each token is an arg. Max of 512 args.
    char* token;
    int clearArray = 0;
    token = strtok(userInput," ");
    if (strstr(token, "#")) commandLineAndInput(userInput, commandLineStruct, numOfArgs, sigFlag); // indicates a comment, return the prompt
    //get the arguments from the user
    int i=0;
    int pidCounter=0;

    while(token != NULL) {

      //perform variable expansion if "$$"
      if (strstr(token, "$$")){
        int pid= getpid();
        char *pidstr;
        {
            int n = snprintf(NULL, 0, "%d", pid);
            pidstr = malloc((n + 1) * sizeof *pidstr);
            sprintf(pidstr, "%d", pid);
        }
        token[strlen(token)-2]='\0'; //strip the $$ from the token
        strcat(token, pidstr); // replace with the pid()
        (*numOfArgs)++;
        commandLineStruct->argsList[i] = calloc(2048, sizeof(char));                      // need to remember to free this!!!!!!!
        strcpy(commandLineStruct->argsList[i], token);
        token= strtok(NULL, " ");
        token= strtok(NULL, " ");
        i++;
      } 

       //if the < redirection is entered, save "<" as the redirect and next token is the filename
      else if(strcmp(token, "<")==0){
        commandLineStruct->inputCmd = true;
        token= strtok(NULL, " ");
        commandLineStruct->inputFile = calloc(2048, sizeof(char));
        strcpy(commandLineStruct->inputFile, token);
        token= strtok(NULL, " ");
      } 

      //if the > redirection is entered, save ">" as the redirect and next token is the filename
      else if(strcmp(token, ">")==0){
        commandLineStruct->outputCmd = true;
        token= strtok(NULL, " ");
        commandLineStruct->outputFile = calloc(2048, sizeof(char));
        strcpy(commandLineStruct->outputFile, token);
        token= strtok(NULL, " ");
      }

      //otherwise just save the token
      else {
          // if the last argument is "&", change to background
          if(strcmp(token, "&")== 0){
            commandLineStruct->backgroundOrForeground = true;
            // however, if we are in foreground only mode- ignore the "&" by turning the bool back to false (0)
            if (sigFlag == 1){
              commandLineStruct->backgroundOrForeground = false;
            }
            break;
          }
          (*numOfArgs)++;
          commandLineStruct->argsList[i] = calloc(2048, sizeof(char));                      // need to remember to free this!!!!!!!
          strcpy(commandLineStruct->argsList[i], token);
          token= strtok(NULL, " ");
          i++;
        }
    }

  }

  free(userInput);
}


/**
 * @brief this function forks off a child process using the execvp() function to run the command. The child process then terminates adfter running the command. Error status and exit status are set to '1' upon failed command.
 * 
 * @param commandLineStruct 
 * @param numOfArgs 
 */
void execProcess(struct commandLine* commandLineStruct, int numOfArgs){
    //worked with OSU tutor on proceeding lines 247 to 256         
    int newArgSize = numOfArgs + 1; // add 1 for the NULL arg
    char* newargv[newArgSize]; //create a new array that will be sent to argvp()
    newargv[newArgSize - 1] = NULL; // set the last element to NULL
    int fD;

    //get every argument and store in the array, stop before last arg of newargv as this must be NULL
    int i=0;
    for (i; i < newArgSize - 1; i++) {
      newargv[i] = commandLineStruct->argsList[i];
    }

    // Fork a new process
    // lines 260 to 327 adopted from Canvas module Exploration: Processes and I/O
    pid_t spawnPid = fork();
    int childStatus;

    switch(spawnPid){
    case -1:
      perror("fork()\n");
      exit(1);
      break;
    case 0: ;
      // In the child process
      
      // handle Ctrl-C command
      struct sigaction SIGINT_action = {0};
      SIGINT_action.sa_handler = SIG_DFL;
      sigaction(SIGINT, &SIGINT_action, NULL);
      
      // input file direction
      if (commandLineStruct->inputCmd == 1) {
        fD = open(commandLineStruct->inputFile, O_RDONLY);
        if (commandLineStruct->backgroundOrForeground==1 && commandLineStruct->inputFile == NULL) fD = open("/dev/null", O_RDONLY); // If the user doesn't redirect the standard input for a background command, then standard input should be redirected to /dev/null
        int result = dup2(fD, STDIN_FILENO);
        if (result == -1) { 
          perror("source open()"); 
          exit(1); 
        }
      }

      // output file direction
      if (commandLineStruct->outputCmd == 1)  {
        fD = open(commandLineStruct->outputFile, O_CREAT | O_RDWR | O_TRUNC, 0777);
        if (commandLineStruct->backgroundOrForeground==1 && commandLineStruct->inputFile == NULL) fD = open("/dev/null", O_CREAT | O_RDWR | O_TRUNC, 0777); // If the user doesn't redirect the standard output for a background command, then standard output should be redirected to /dev/null
        int result = dup2(fD, STDOUT_FILENO);
        if (result == -1){
          perror("source open()"); 
          exit(1); 
        }
      }

      execvp(newargv[0], newargv);
      // exec only returns if there is an error
      perror(newargv[0]); // display the name of the command that execvp() could not run
      exit(1);
      break;

    default:
      // In the parent process
      //Foreground process, wait for process to end before advancing
      if(commandLineStruct->backgroundOrForeground==0){
        spawnPid = waitpid(spawnPid, &childStatus, 0); // foreground
        if(WIFSIGNALED(childStatus)){
          printf("terminated by signal %d\n", WTERMSIG(childStatus));
        }
        commandLineStruct->status = childStatus; // this will be used for the UpdateStatus() function

      } 
      //background process, do not wait but check status of child later on
      else if (commandLineStruct->backgroundOrForeground==1) {
        waitpid(spawnPid, &childStatus, WNOHANG); // background
        printf("background pid is %d\n", spawnPid); //if it is, print the background pid
        if(WIFSIGNALED(childStatus)){
          printf("terminated by signal %d\n", WTERMSIG(childStatus));
        }
        commandLineStruct->status = childStatus; // this will be used for the UpdateStatus() function
        pidList[pidIncrement] = spawnPid;
        pidIncrement++;
      }  
      break;
    }
    
}


/**
 * @brief This is the main function.
 * 
 * @return int 
 */
int main() {
  // worked with OSU tutor on lines 340 to 349
  // handle Ctrl-Z command
  struct sigaction SIGTSTP_action = {0};
  SIGTSTP_action.sa_handler = handle_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_RESTART;
  sigaction(SIGTSTP, &SIGTSTP_action, NULL);

  // handle Ctrl-C command
  struct sigaction SIGINT_action = {0};
  SIGINT_action.sa_handler = SIG_IGN;
  sigaction(SIGINT, &SIGINT_action, NULL);


  //to be passed to command line function
  int numOfArgs;
  char* userInput = NULL;
  struct commandLine* commandLineStruct= malloc(sizeof(struct commandLine));            //freed when exitShell() is called

  while(1){
    //call the prompt and get user input
    numOfArgs = 0;
    commandLineAndInput(userInput, commandLineStruct, &numOfArgs, sigFlag);
    fflush(stdout);

    //if the user enters "cd" then change directory
    if (strcmp(commandLineStruct->argsList[0],"cd")==0){
      CD(commandLineStruct, numOfArgs);
    }

    //if the user enters "exit", then the shell terminates and exits
    else if (strcmp(commandLineStruct->argsList[0], "exit")== 0){
      free(commandLineStruct); //free the allocated memory for the struct
      exitShell();
    }

    //if the user enteres "status", then the shell returns the status
    else if (strcmp(commandLineStruct->argsList[0], "status")==0){
      updateStatus(commandLineStruct);
    }
    
    // send the command to exec() and let process be handled outside of shell
    else {
      execProcess(commandLineStruct, numOfArgs);
    }
    
  }

  // if we reach here, something happened and the shell did not run correctly/crashed
  return EXIT_FAILURE;
}

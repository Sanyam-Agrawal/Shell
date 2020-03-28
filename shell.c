#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <pwd.h>

#define MAX_INPUT_SIZE 10000
#define MAX_ARGS       1000

#define sep(x) (isspace(x) || (x)=='<' || (x)=='>' || (x)=='|' || (x)==';')

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define START_ERR      write(2, ANSI_COLOR_RED,   strlen(ANSI_COLOR_RED)  );
#define STOP_ERR       write(2, ANSI_COLOR_RESET, strlen(ANSI_COLOR_RESET));
#define WRITE_ERROR(x) do { START_ERR; write(2, x, strlen(x)); STOP_ERR; } while(0)
#define PERROR_SHIM(x) do { START_ERR; perror(x);              STOP_ERR; } while(0)

#define ERROR          do { error_flag = 1; goto end; } while(0)
#define WERROR(x)      do { WRITE_ERROR(x); ERROR; } while(0)
#define PERROR         do { PERROR_SHIM("ERROR "); ERROR; } while(0)
#define EXITERROR      do { PERROR_SHIM("ERROR "); exit(EXIT_FAILURE); } while(0)

char* get_text(char input[], int input_length, int *in){
	// find length of text
	int text_length = 0, end_ptr = *in;
	int quotes_flag = 0;
	while(end_ptr < input_length){
		// end of text
		if(!quotes_flag && sep(input[end_ptr])) break;

		// handling quotes
		if(input[end_ptr] == '\"') quotes_flag = 1 - quotes_flag;
		// handling escapes
		else if(input[end_ptr] == '\\'){
			// nothing after '''\'''
			if(end_ptr+1 == input_length){
				WRITE_ERROR("ERROR: Nothing after \\\n");
				return NULL;
			}
			++text_length;
			++end_ptr;
		}
		// normal text
		else{
			++text_length;
		}

		++end_ptr;
	}

	// a opening " without closing
	if(quotes_flag){
		WRITE_ERROR("ERROR: Missing closing \"\n");
		return NULL;
	}

	// allocate buffer
	char* ret = malloc(sizeof(char) * (text_length+1));
	if(ret == NULL) EXITERROR;

	// copy text into buffer
	int ptr_buf = 0;
	while(*in < end_ptr){
		if(input[*in] == '\"'){ ++(*in); continue; }        // ", go ahead
		if(input[*in] == '\\') ++(*in);                     // '''\''', take next
		if(*in >= end_ptr || ptr_buf >= text_length) break; // length exceeded, break
		ret[ptr_buf++] = input[(*in)++];                    // copy into buf
	}
	ret[ptr_buf] = '\0';

	return ret;
}

int cd(char* path){
	// cd `dir` defaults to $HOME
	if(path == NULL){
		if((path = getenv("HOME")) == NULL){
			struct passwd *p = getpwuid(getuid());
			if(p == NULL){
				PERROR_SHIM("ERROR ");
				return -1;
			}
			path = p -> pw_dir;
		}
	}

	// change working directory
	if(chdir(path) < 0){
		PERROR_SHIM("ERROR ");
		return -1;
	}

	int retvalue = 0;

	// change $PWD
	char *new_p = getcwd(NULL,0);
	if(new_p == NULL || setenv("PWD", new_p, 1) < 0){
		PERROR_SHIM("ERROR ");
		retvalue = -1;
	}
	free(new_p);

	return retvalue;
}

void quit(){
	char* exit_msg = ANSI_COLOR_BLUE "\nKTHNXBYE\n" ANSI_COLOR_RESET;
	write(1, exit_msg, strlen(exit_msg));
	exit(EXIT_SUCCESS);
}

char* args[MAX_ARGS+1];
int args_no;

void execute_command(char* input, int input_length){
	// boolean, whether command contains error
	int error_flag = 0;

	// Counting no of pipes in input command
	int npipes = 0;
	{
		int quotes_flag = 0, backslash_flag = 0;
		for(int i=0; i<input_length; ++i){
			if (backslash_flag)        backslash_flag = 0;
			else if (input[i] == '\"') quotes_flag = 1 - quotes_flag;
			else if (input[i] == '\\') backslash_flag = 1;
			else if (!quotes_flag && input[i] == '|')  ++npipes;
		}
	}

	// Creating pipes for each piping
	int fd[npipes ? npipes : 1][2];
	for(int p=0; p<npipes; ++p) if(pipe(fd[p]) < 0) EXITERROR;

	int in = 0;

	for(int p=0; p<=npipes; ++p){
		// set-up
		int infd = 0, outfd = 1, errfd = 2;
		args[0] = NULL;
		args_no = 1;

		// PARSING
		while(in<input_length){
			// at beginning of next piped segment
			if (input[in] == '|'){
				++in;
				break;
			}

			// whitespace
			if (isspace(input[in])){
				++in;
				continue;
			}

			switch(input[in]){
				case '>':
				{
					// distinguish stdout and stderr
					int stderr_flag = 0;
					if(0<in && input[in-1]=='2' && (in<2 || isspace(input[in-2]) || input[in-2]=='|'))
						stderr_flag = 1;

					// distinguish > and >>
					int append_flag = 0;
					if(in+1<input_length && input[in+1]=='>') append_flag = 1;

					// increase pointer to input accordingly
					in += 1+append_flag;

					// check for 2>&1
					if(stderr_flag && in+1<input_length && input[in]=='&' && input[in+1]=='1'){
						in += 2;
						errfd = outfd;
						break;
					}

					// remove whitespace
					while(in<input_length && isspace(input[in])) ++in;

					// filename missing
					if(in+1>=input_length || sep(input[in]))
						WERROR("ERROR: Filename missing after >\n");

					// get filename
					char* outfile = get_text(input, input_length, &in);
					if(outfile == NULL) ERROR;

					if(stderr_flag){
						// if already redirected, close previous
						if(errfd>2 && errfd!=outfd && close(errfd) < 0) PERROR;
						// get file descriptor
						if((errfd = open(outfile, O_WRONLY | O_CREAT |
						                 (append_flag ? O_APPEND : O_TRUNC), 0644)) < 0)
						{
							// file error
							PERROR_SHIM("ERROR, culprit = STDERR Redirection ");
							free(outfile);
							ERROR;
						}
					}
					else{
						// if already redirected, close previous
						if(outfd>2 && outfd!=errfd && close(outfd) < 0) PERROR;
						// get file descriptor
						if((outfd = open(outfile, O_WRONLY | O_CREAT |
						                 (append_flag ? O_APPEND : O_TRUNC), 0644)) < 0)
						{
							// file error
							PERROR_SHIM("ERROR, culprit = STDOUT Redirection ");
							free(outfile);
							ERROR;
						}
					}
					free(outfile);
				}
				break;

				case '<':
				{
					// take pointer to start of filename
					++in;
					while(in<input_length && isspace(input[in])) ++in;

					// filename missing
					if(in+1>=input_length || sep(input[in]))
						WERROR("ERROR: Filename missing after <\n");

					// get filename
					char* infile = get_text(input, input_length, &in);
					if(infile == NULL) ERROR;

					// if already redirected, close previous
					if(infd>2 && close(infd) < 0) PERROR;

					// get file descriptor
					if((infd = open(infile, O_RDONLY)) < 0){
						// file error
						PERROR_SHIM("ERROR, culprit = STDIN Redirection ");
						free(infile);
						ERROR;
					}
					free(infile);
				}
				break;

				case '1':
				case '2':
					// check if this is for redirection
					if(in+1<input_length && input[in+1]=='>'){
						++in;
						break;
					}

				default:
				{
					// command not yet parsed
					if(args[0] == NULL){
						if((args[0] = get_text(input, input_length, &in)) == NULL) ERROR;
					}
					// must be an argument
					else{
						if(args_no == MAX_ARGS) WERROR("ERROR, culprit = Too Many Args\n");
						if((args[args_no++] = get_text(input, input_length, &in)) == NULL) ERROR;
					}
				}
			}
		}

		// if no command is given, then give an empty string
		if(args[0] == NULL) args[0] = calloc(1, sizeof(char));

		// execvp requires NULL-terminated arrays
		args[args_no] = NULL;

		// `exit`
		if(! strcmp(args[0], "exit")) quit();

		// `cd`
		if(! strcmp(args[0], "cd")){
			if(args_no>2) WERROR("ERROR, culprit = `cd` has atmost one argument.\n");
			if(cd((args_no==1) ? NULL : args[1]) == -1) ERROR;
			goto end;
		}

		// create child to exec command
		int pid = fork();

		if (pid > 0){
			// Parent Process

			// waiting for child to exit
			wait(NULL);

			// close reading end of previous pipe, as cleanup
			if(p != 0 && close(fd[p-1][0]) < 0) PERROR;

			// close writing end of pipe to enable EOF
			if(p != npipes && close(fd[p][1]) < 0) PERROR;
		}

		else if (pid == 0){
			// Child Process

			if(infd>2){
				// STDIN redirected
				if(close(0) < 0) EXITERROR;
				if(dup(infd) < 0) EXITERROR;
				if(close(infd) < 0) EXITERROR;
			}
			else if(p != 0){
				// STDIN takes from previous pipe
				if(close(0) < 0) EXITERROR;
				if(dup(fd[p-1][0]) < 0) EXITERROR;
				if(close(fd[p-1][0]) < 0) EXITERROR;
			}

			if(outfd>2){
				// STDOUT redirected
				if(close(1) < 0) EXITERROR;
				if(dup(outfd) < 0) EXITERROR;
				// maybe Stderr is redirected to the same place
				//       so only close if not 
				if(outfd != errfd && close(outfd) < 0) EXITERROR;
			}
			else if(p != npipes){
				// STDOUT gives to next pipe
				if(close(fd[p][0]) < 0) EXITERROR;
				if(close(1) < 0) EXITERROR;
				if(dup(fd[p][1]) < 0) EXITERROR;
				if(close(fd[p][1]) < 0) EXITERROR;
			}

			if(errfd>2){
				// STDERR redirected
				if(close(2) < 0) EXITERROR;
				if(dup(errfd) < 0) EXITERROR;
				if(close(errfd) < 0) EXITERROR;
			}

			// empty command, so nothing to execute
			if(args[0][0]=='\0') exit(EXIT_SUCCESS);

			// finally!
			execvp(args[0],args);

			// exec failed
			EXITERROR;
		}

		else{
			// fork failed
			PERROR;
		}

		end:
			// deallocate memory to avoid memory leaks
			for(int i=0; i<args_no; ++i) free(args[i]);
			// error in command so stop
			if(error_flag) return;
	}
}

int main (void){
	char input[MAX_INPUT_SIZE+1];
	int input_length;

	while(1){
		// Print stuff
		char* welcome_msg = ANSI_COLOR_GREEN "$ " ANSI_COLOR_RESET;
		write(1, welcome_msg, strlen(welcome_msg));

		// Getting input from user
		if((input_length = read(0, input, MAX_INPUT_SIZE)) < 0){
			PERROR_SHIM("ERROR ");
			continue;
		}

		// EOF
		if(input_length == 0) quit();
		// '\n' is not part of command, so if present, strip it
		if(input[input_length-1] == '\n') --input_length;

		char* ptr = input;
		int prev = 0;
		int quotes_flag = 0, backslash_flag = 0;
		for(int i=0; i<input_length; ++i){
			if (backslash_flag)        backslash_flag = 0;
			else if (input[i] == '\"') quotes_flag = 1 - quotes_flag;
			else if (input[i] == '\\') backslash_flag = 1;
			else if (!quotes_flag && input[i] == ';'){
				execute_command(ptr, i-prev);
				ptr = input+i+1;
				prev = i+1;
			}
		}

		// some input left to execute
		if(0<input_length-prev && (ptr + input_length-prev <= input + input_length))
			execute_command(ptr, input_length-prev);

		write(1, "\n", 1);
	}
}

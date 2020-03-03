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

#define sep(x) (isspace(x) || (x)=='<' || (x)=='>' || (x)=='|')
#define ERROR  { error_flag = 1; goto end; }

char* get_text(char input[], int input_length, int *in){
	// find length of text
	int text_length = 0;
	while(*in+text_length < input_length && !sep(input[*in+text_length])) ++text_length;

	// allocate buffer
	char* ret = malloc(sizeof(char) * (text_length+1));
	if(ret == NULL){
		// malloc failed
		perror("FATAL ERROR : ");
		exit(EXIT_FAILURE);
	}

	// copy text into buffer
	for(int c=0; c<text_length; ++c) ret[c] = input[(*in)++];
	ret[text_length] = '\0';

	return ret;
}

int cd(char* path){
	// cd `dir` defaults to $HOME
	if(path == NULL){
		if((path = getenv("HOME")) == NULL){
			struct passwd *p = getpwuid(getuid());
			if(p == NULL){
				perror("ERROR ");
				return -1;
			}
			path = p -> pw_dir;
		}
	}

	// change working directory
	int ret_code = chdir(path);
	if(ret_code == -1){
		perror("ERROR ");
		return -1;
	}

	// change $PWD
	char *new_p = getcwd(NULL,0);
	setenv("PWD", new_p, 1);
	free(new_p);

	return 0;
}

int main (void){
	char input[MAX_INPUT_SIZE+1];
	int input_length;

	char* args[MAX_ARGS+1];
	int args_no;

	while(1){
		// Getting input from user
		write(1, "$ ", 2);
		input_length = read(0, input, MAX_INPUT_SIZE) - 1;
		int in = 0;

		// boolean, whether command contains error
		int error_flag = 0;

		// Counting no of pipes in input command
		int npipes = 0;
		for(int i=0; i<input_length; ++i)
			if (input[i] == '|')
				++npipes;

		// Creating pipes for each piping
		int fd[npipes ? npipes : 1][2];
		for(int p=0; p<npipes; ++p) pipe(fd[p]);

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

				if (isspace(input[in])){
					++in;
					continue;
				}

				switch(input[in]){
					case '>':
						{
							// distinguish stdout and stderr
							int stderr_flag = 0;
							if(0<in && input[in-1]=='2' && (in<2 || isspace(input[in-2]) || input[in-2]=='|')) stderr_flag = 1;

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
							if(in+1>=input_length || sep(input[in])){
								write(2, "ERROR: Filename missing after >\n", 32);
								ERROR;
							}

							// get file descriptor
							char* outfile = get_text(input, input_length, &in);
							if(stderr_flag){
								if(errfd>2 && errfd!=outfd) close(errfd);
								errfd = open(outfile, O_WRONLY | O_CREAT | (append_flag ? O_APPEND : O_TRUNC), 0644);
								if(errfd<0){
									// file error
									perror("ERROR, culprit = STDERR Redirection ");
									free(outfile);
									ERROR;
								}
							}
							else{
								if(outfd>2 && outfd!=errfd) close(outfd);
								outfd = open(outfile, O_WRONLY | O_CREAT | (append_flag ? O_APPEND : O_TRUNC), 0644);
								if(outfd<0){
									// file error
									perror("ERROR, culprit = STDOUT Redirection ");
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
							if(in+1>=input_length || sep(input[in])){
								write(2, "ERROR: Filename missing after <\n", 32);
								ERROR;
							}

							// get file descriptor
							char* infile = get_text(input, input_length, &in);
							if(infd>2) close(infd);
							infd = open(infile, O_RDONLY);
							if(infd<0){
								// file error
								perror("ERROR, culprit = STDIN Redirection ");
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
							args[0] = get_text(input, input_length, &in);
						}
						// must be an argument
						else{
							if(args_no == MAX_ARGS){
								// this many args cannot fit in our buffer
								write(2, "ERROR, culprit = Too Many Args\n", 31);
								ERROR;
							}
							args[args_no++] = get_text(input, input_length, &in);
						}
					}
				}
			}

			// if no command is given, then give an empty string
			if(args[0] == NULL){
				args[0] = malloc(sizeof(char));
				args[0][0] = '\0';
			}

			// execvp requires NULL-terminated arrays
			args[args_no] = NULL;

			// `exit`
			if(! strcmp(args[0], "exit")) exit(EXIT_SUCCESS);

			if(! strcmp(args[0], "cd")){
				if(args_no>2){
					write(2, "ERROR, culprit = `cd` has atmost one argument.\n", 47);
					ERROR;
				}
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
				if(p != 0) close(fd[p-1][0]);

				// close writing end of pipe to enable EOF
				if(p != npipes) close(fd[p][1]);
			}

			else if (pid == 0){
				// Child Process

				if(infd>2){
					// STDIN redirected
					close(0);
					dup(infd);
					close(infd);
				}
				else if(p != 0){
					// STDIN takes from previous pipe
					close(0);
					dup(fd[p-1][0]);
					close(fd[p-1][0]);
				}

				if(outfd>2){
					// STDOUT redirected
					close(1);
					dup(outfd);
					// maybe Stderr is redirected to the same place
					//       so only close if not 
					if(outfd != errfd) close(outfd);
				}
				else if(p != npipes){
					// STDOUT gives to next pipe
					close(fd[p][0]);
					close(1);
					dup(fd[p][1]);
					close(fd[p][1]);
				}

				if(errfd>2){
					// STDERR redirected
					close(2);
					dup(errfd);
					close(errfd);
				}

				// empty command, so nothing to execute
				if(args[0][0]=='\0') exit(EXIT_SUCCESS);

				// finally!
				execvp(args[0],args);

				// exec failed
				perror("ERROR ");
				error_flag = 1;
				exit(EXIT_FAILURE);
			}

			else{
				// fork failed
				perror("ERROR ");
				ERROR;
			}

			end:
				// deallocate memory to avoid memory leaks
				for(int i=0; i<args_no; ++i) free(args[i]);
				// error in command so stop
				if(error_flag) break;
		}

		write(1, "\n", 1);
	}
}

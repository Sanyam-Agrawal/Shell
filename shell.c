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
				write(2, "ERROR: Nothing after \\\n", 23);
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
		write(2, "ERROR: Missing closing \"\n", 25);
		return NULL;
	}

	// allocate buffer
	char* ret = malloc(sizeof(char) * (text_length+1));
	if(ret == NULL){
		// malloc failed
		perror("FATAL ERROR ");
		exit(EXIT_FAILURE);
	}

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

		// EOF
		if(input_length == -1) exit(EXIT_SUCCESS);

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

							// get filename
							char* outfile = get_text(input, input_length, &in);
							if(outfile == NULL) ERROR;

							if(stderr_flag){
								// if already redirected, close previous
								if(errfd>2 && errfd!=outfd) close(errfd);
								// get file descriptor
								errfd = open(outfile, O_WRONLY | O_CREAT | (append_flag ? O_APPEND : O_TRUNC), 0644);
								if(errfd<0){
									// file error
									perror("ERROR, culprit = STDERR Redirection ");
									free(outfile);
									ERROR;
								}
							}
							else{
								// if already redirected, close previous
								if(outfd>2 && outfd!=errfd) close(outfd);
								// get file descriptor
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

							// get filename
							char* infile = get_text(input, input_length, &in);
							if(infile == NULL) ERROR;

							// if already redirected, close previous
							if(infd>2) close(infd);

							// get file descriptor
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
							if(args[0] == NULL) ERROR;
						}
						// must be an argument
						else{
							if(args_no == MAX_ARGS){
								// this many args cannot fit in our buffer
								write(2, "ERROR, culprit = Too Many Args\n", 31);
								ERROR;
							}
							args[args_no++] = get_text(input, input_length, &in);
							if(args[args_no-1] == NULL) ERROR;
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

			// `cd`
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

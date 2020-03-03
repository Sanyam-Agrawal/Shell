run: shell
	./shell

shell: shell.c
	${CC} -o $@ $^

.PHONY: clean

clean:
	${RM} shell

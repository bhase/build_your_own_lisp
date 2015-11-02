
all: prompt parsing eval

prompt: prompt.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

parsing: parsing.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

eval: eval.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

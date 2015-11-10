
all: prompt parsing eval error_handling s_expression q_expression

prompt: prompt.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

parsing: parsing.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

eval: eval.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

error_handling: error_handling.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

s_expression: s_expression.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

q_expression: q_expression.c mpc.c
	gcc -Wall -std=c99 $^ -lm -lreadline -o $@

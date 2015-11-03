#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "mpc.h"

enum { LVAL_NUM, LVAL_ERR };
enum { LERR_DIV_ZERO, LERR_BAD_OP, LERR_BAD_NUM };

typedef struct {
	int type;
	long num;
	int err;
} lval;

static lval lval_num(long x) {
	lval v;
	v.type = LVAL_NUM;
	v.num = x;
	return v;
}

static lval lval_err(int err) {
	lval v;
	v.type = LVAL_ERR;
	v.err = err;
	return v;
}

static void lval_print(lval v) {
	switch (v.type) {
	case LVAL_NUM:
	       	printf("%li", v.num);
	       	break;
	case LVAL_ERR:
		if (v.err == LERR_DIV_ZERO) {
			printf("Error: Division by zero!");
		}
		if (v.err == LERR_BAD_OP) {
			printf("Error: Invalid operator!");
		}
		if (v.err == LERR_BAD_NUM) {
			printf("Error: Invalid number!");
		}
		break;
	}
}

static void lval_println(lval v) {
	lval_print(v);
	putchar('\n');
}

static lval eval_op(lval x, char *op, lval y) {

	if (x.type == LVAL_ERR) {
		return x;
	}
	if (y.type == LVAL_ERR) {
		return y;
	}

	if (strcmp(op, "+") == 0) { return lval_num(x.num + y.num); }
	if (strcmp(op, "-") == 0) { return lval_num(x.num - y.num); }
	if (strcmp(op, "*") == 0) { return lval_num(x.num * y.num); }
	if (strcmp(op, "/") == 0) {
		if (y.num == 0) {
			return lval_err(LERR_DIV_ZERO);
		} else {
			return lval_num(x.num / y.num); 
		}
	}
	if (strcmp(op, "%") == 0) {
		if (y.num == 0) {
			return lval_err(LERR_DIV_ZERO);
		} else {
			return lval_num(x.num % y.num); 
		}
	}
	if (strcmp(op, "^") == 0) { return lval_num(pow(x.num, y.num)); }
	return lval_err(LERR_BAD_OP);
}

static lval eval(mpc_ast_t *t) {

	/* If tagged as number, return it directly */
	if (strstr(t->tag, "number")) {
		/* check for conversion errors */
		errno = 0;
		long x = strtol(t->contents, NULL, 10);
		return errno != ERANGE ? lval_num(x) : lval_err(LERR_BAD_NUM);
	}

	/* The operator is always second child */
	char *op = t->children[1]->contents;

	/* store third child in x */
	lval x = eval(t->children[2]);

	/* iterate the remaining children */
	int i = 3;
	while (strstr(t->children[i]->tag, "expr")) {
		x = eval_op(x, op, eval(t->children[i]));
		i++;
	}
	return x;
}

int main(int argc, char** argv) {

	/* Create some parsers */
	mpc_parser_t *Number   = mpc_new("number");
	mpc_parser_t *Operator = mpc_new("operator");
	mpc_parser_t *Expr     = mpc_new("expr");
	mpc_parser_t *Lispy    = mpc_new("lispy");

	/* Define them with the following language */
	mpca_lang(MPCA_LANG_DEFAULT,
		  ""
		  "number   : /-?[0-9]+/ ('.' /[0-9]+/)? ;"
		  "operator : '+' | \"add\" | '-' | \"sub\""
		  "         | '*' | \"mul\" | '/' | \"div\""
		  "         | '%' | \"mod\" | '^' ;"
		  "expr     : <number> | '(' <operator> <expr>+ ')' ;"
		  "lispy    : /^/ <operator> <expr>+ /$/ ; "
		  "",
		  Number, Operator, Expr, Lispy);

	/* Print Version and Exit Information */
	puts("Lispy Version 0.0.0.0.1");
	puts("Press Ctrl+c or Ctrl+d to Exit\n");

	/* In a never ending loop */
	while (1) {
		/* Output our prompt and get input */
		char* input = readline("lispy> ");
		if (NULL == input) {
			/* quit with Ctrl-d */
			break;
		}

		/* Add input to history */
		add_history(input);

		/* Try to parse the user input */
		mpc_result_t r;
		if (mpc_parse("<stdin>", input, Lispy, &r)) {
			/* On success evaluate AST */
			lval result = eval(r.output);
			lval_println(result);
			mpc_ast_delete(r.output);
		} else {
			mpc_err_print(r.error);
			mpc_err_delete(r.error);
		}

		/* Free retrieved input */
		free(input);
	}

	/* Undefine and delete the parsers */
	mpc_cleanup(4, Number, Operator, Expr, Lispy);

	return 0;
}

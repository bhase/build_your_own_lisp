#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "mpc.h"

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
		  "         | '%' | \"mod\" ;"
		  "expr     : <number> | '(' <operator> <expr>+ ')' ;"
		  "lispy    : /^/ <operator> <expr>+ /$/ ; "
		  "",
		  Number, Operator, Expr, Lispy);

	/* Print Version and Exit Information */
	puts("Lispy Version 0.0.0.0.1");
	puts("Press Ctrl+c to Exit\n");

	/* In a never ending loop */
	while (1) {
		/* Output our prompt and get input */
		char* input = readline("lispy> ");

		/* Add input to history */
		add_history(input);

		/* Try to parse the user input */
		mpc_result_t r;
		if (mpc_parse("<stdin>", input, Lispy, &r)) {
			/* On success print AST */
			mpc_ast_print(r.output);
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

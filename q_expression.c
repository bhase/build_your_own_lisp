#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "mpc.h"

#define LASSERT(arg, cond, err) \
	if (!(cond)) { lval_del(arg); return lval_err(err); }

enum { LVAL_ERR, LVAL_NUM, LVAL_SYM, LVAL_SEXPR, LVAL_QEXPR };
enum { LERR_DIV_ZERO, LERR_BAD_OP, LERR_BAD_NUM };

typedef struct lval {
	int type;
	long num;
	/* error and symbol types have string contents */
	char *err;
	char *sym;
	/* count and pointer to a list of lval */
	int count;
	struct lval **cell;
} lval;

static lval * lval_num(long x) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_NUM;
	v->num = x;
	return v;
}

static lval * lval_err(char *err) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_ERR;
	v->err = malloc(strlen(err) + 1);
	strcpy(v->err, err);
	return v;
}

static lval * lval_sym(char *sym) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_SYM;
	v->sym = malloc(strlen(sym) + 1);
	strcpy(v->sym, sym);
	return v;
}

static lval * lval_sexpr(void) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_SEXPR;
	v->count = 0;
	v->cell = NULL;
	return v;
}

static lval * lval_qexpr(void) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_QEXPR;
	v->count = 0;
	v->cell = NULL;
	return v;
}

static void lval_del(lval *v) {
	switch (v->type) {
	/* no special handling for numbers */
	case LVAL_NUM: break;
	case LVAL_ERR:
		       free(v->err);
		       break;
	case LVAL_SYM:
		       free(v->sym);
		       break;
	case LVAL_QEXPR: /* no break! */
	case LVAL_SEXPR:
		       for (int i = 0; i < v->count; i++) {
			       lval_del(v->cell[i]);
		       }
		       free(v->cell);
		       break;
	}
	free(v);
}

static lval * lval_read_num(mpc_ast_t *t) {
	errno = 0;
	long x = strtol(t->contents, NULL, 10);
	return errno != ERANGE ? lval_num(x) : lval_err("invalid number");
}

static lval * lval_add(lval *v, lval *x) {
	v->count++;
	v->cell = realloc(v->cell, sizeof(lval *) * v->count);
	v->cell[v->count - 1] = x;
	return v;
}

static lval * lval_read(mpc_ast_t *t) {

	if (strstr(t->tag, "number")) { return lval_read_num(t); }
	if (strstr(t->tag, "symbol")) { return lval_sym(t->contents); }

	/* if root (>) or sexpr then create empty list */
	lval *x = NULL;
	if (strcmp(t->tag, ">") == 0) { x = lval_sexpr(); }
	if (strstr(t->tag, "sexpr"))  { x = lval_sexpr(); }
	if (strstr(t->tag, "qexpr"))  { x = lval_qexpr(); }

	/* fill this list with any valid expression contained within */
	for (int i = 0; i < t->children_num; i++) {
		if (strcmp(t->children[i]->contents, "(") == 0) { continue; }
		if (strcmp(t->children[i]->contents, ")") == 0) { continue; }
		if (strcmp(t->children[i]->contents, "{") == 0) { continue; }
		if (strcmp(t->children[i]->contents, "}") == 0) { continue; }
		if (strcmp(t->children[i]->tag,  "regex") == 0) { continue; }
		x = lval_add(x, lval_read(t->children[i]));
	}

	return x;
}

static void lval_print(lval *v);
static void lval_expr_print(lval *v, char open, char close) {
	putchar(open);
	for (int i = 0; i < v->count; i++) {
		lval_print(v->cell[i]);

		/* don't print trailing space if last element */
		if (i != (v->count - 1)) {
			putchar(' ');
		}
	}
	putchar(close);
}

static void lval_print(lval *v) {
	switch (v->type) {
	case LVAL_NUM:
	       	printf("%li", v->num);
	       	break;
	case LVAL_ERR:
		printf("Error: %s", v->err);
		break;
	case LVAL_SYM:
		printf("%s", v->sym);
		break;
	case LVAL_SEXPR:
		lval_expr_print(v, '(', ')');
		break;
	case LVAL_QEXPR:
		lval_expr_print(v, '{', '}');
		break;
	}
}

static void lval_println(lval *v) {
	lval_print(v);
	putchar('\n');
}


static lval * lval_pop(lval *v, int i) {
	/* Find the item at "i" */
	lval *x = v->cell[i];

	/* Shift memory after the item at i over the top */
	memmove(&v->cell[i], &v->cell[i + 1], sizeof(lval *) * (v->count - i - 1));

	/* Decrease the count of items in the list */
	v->count--;

	/* reallocate the memory used */
	v->cell = realloc(v->cell, sizeof(lval *) * v->count);
	return x;
}

static lval * lval_take(lval *v, int i) {
	lval *x = lval_pop(v, i);
	lval_del(v);
	return x;
}

static lval * builtin_head(lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'head' passed too many arguments!");
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'head' passed incorrect type!");
	LASSERT(a, a->cell[0]->count != 0,
		"Function 'head' passed {}!");

	lval *v = lval_take(a, 0);
	while(v->count > 1) {
		lval_del(lval_pop(v, 1));
	}
	return v;
}

static lval * builtin_tail(lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'tail' passed too many arguments!");
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'tail' passed incorrect type!");
	LASSERT(a, a->cell[0]->count != 0,
		"Function 'tail' passed {}!");

	lval *v = lval_take(a, 0);
	lval_del(lval_pop(v, 0));
	return v;
}

static lval * builtin_list(lval *a) {
	a->type = LVAL_QEXPR;
	return a;
}

static lval * lval_eval(lval *x);
static lval * builtin_eval(lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'eval' passed too many arguments!");
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'eval' passed incorrect type!");

	lval *x = lval_take(a, 0);
	x->type = LVAL_SEXPR;
	return lval_eval(x);
}

static lval * lval_join(lval *x, lval *y) {

	/* for each cell in 'y' add it to 'x' */
	while (y->count) {
		x = lval_add(x, lval_pop(y, 0));
	}

	/* Delete empty 'y' and return 'x' */
	lval_del(y);
	return x;
}

static lval * builtin_join(lval *a) {

	for (int i = 0; i < a->count; i++) {
		LASSERT(a, a->cell[i]->type == LVAL_QEXPR,
			"Function 'join' passed incorrect type!");
	}

	lval *x = lval_pop(a, 0);
	while (a->count) {
		x = lval_join(x, lval_pop(a, 0));
	}

	lval_del(a);
	return x;
}

static lval * builtin_op(lval *a, char *op) {
	
	/* Ensure all arguments are numbers */
	for (int i = 0; i < a->count; i++) {
		if (a->cell[i]->type != LVAL_NUM) {
			lval_del(a);
			return lval_err("Cannot operate on non-number!");
		}
	}

	/* Pop the first element */
	lval *x = lval_pop(a, 0);

	/* If no arguments and sub then perform unary negation */
	if (a->count == 0 && (strcmp(op, "-") == 0)) {
		x->num = -x->num;
	}

	/* while there are still elements remaining */
	while (a->count > 0) {

		/* pop the next element */
		lval *y = lval_pop(a, 0);

		if (strcmp(op, "+") == 0) {
			x->num += y->num;
		} else if (strcmp(op, "-") == 0) {
			x->num -= y->num;
		} else if (strcmp(op, "*") == 0) {
			x->num *= y->num;
		} else if (strcmp(op, "/") == 0) {
			if (y->num == 0) {
				lval_del(x);
				lval_del(y);
				x = lval_err("Division by zero!");
				break;
			}
			x->num /= y->num;
		} else if (strcmp(op, "%") == 0) {
			if (y->num == 0) {
				lval_del(x);
				lval_del(y);
				x = lval_err("Division by zero!");
				break;
			}
			x->num %= y->num;
		} else if (strcmp(op, "^") == 0) {
			x->num = pow(x->num, y->num);
		} else {
			lval_del(x);
			lval_del(y);
			x = lval_err("unknown operator!");
			break;
		}

		lval_del(y);
	}

	lval_del(a);
	return x;
}

static lval * builtin(lval *a, char *func) {
	if (strcmp("list", func) == 0) { return builtin_list(a); }
	if (strcmp("head", func) == 0) { return builtin_head(a); }
	if (strcmp("tail", func) == 0) { return builtin_tail(a); }
	if (strcmp("join", func) == 0) { return builtin_join(a); }
	if (strcmp("eval", func) == 0) { return builtin_eval(a); }
	if (strstr("+-/*%^", func)) { return builtin_op(a, func); }
	lval_del(a);
	return lval_err("Unknown Function!");
}


static lval * lval_eval(lval *v);
static lval * lval_eval_sexpr(lval *v) {

	/* Evaluate children */
	for (int i = 0; i < v->count; i++) {
		v->cell[i] = lval_eval(v->cell[i]);
	}

	/* Error checking */
	for (int i = 0; i < v->count; i++) {
		if (v->cell[i]->type == LVAL_ERR) {
			return lval_take(v, i);
		}
	}

	/* Empty expression */
	if (v->count == 0) {
		return v;
	}

	/* Single expression */
	if (v->count == 1) {
		return lval_take(v, 0);
	}

	/* Ensure fist element is symbol */
	lval *f = lval_pop(v, 0);
	if (f->type != LVAL_SYM) {
		lval_del(f);
		lval_del(v);
		return lval_err("S-Expression does not start with symbol!");
	}

	/* Call builtin with operator */
	lval *result = builtin(v, f->sym);
	lval_del(f);
	return result;
}

static lval * lval_eval(lval *v) {
	/* Evaluate sexpressions */
	if (v->type == LVAL_SEXPR) {
		return lval_eval_sexpr(v);
	}
	/* All other lval types remain the same */
	return v;
}

int main(int argc, char** argv) {

	/* Create some parsers */
	mpc_parser_t *Number   = mpc_new("number");
	mpc_parser_t *Symbol   = mpc_new("symbol");
	mpc_parser_t *Sexpr    = mpc_new("sexpr");
	mpc_parser_t *Qexpr    = mpc_new("qexpr");
	mpc_parser_t *Expr     = mpc_new("expr");
	mpc_parser_t *Lispy    = mpc_new("lispy");

	/* Define them with the following language */
	mpca_lang(MPCA_LANG_DEFAULT,
		  ""
		  "number   : /-?[0-9]+/ ;"
		  "symbol   : '+' | '-' | '*' | '/' | '%' | '^' "
		  "         | \"list\" | \"head\" | \"tail\" | \"join\" | \"eval\" ;"
		  "sexpr    : '(' <expr>* ')' ;"
		  "qexpr    : '{' <expr>* '}' ;"
		  "expr     : <number> | <symbol> | <sexpr> | <qexpr> ;"
		  "lispy    : /^/ <expr>* /$/ ; "
		  "",
		  Number, Symbol, Sexpr, Qexpr, Expr, Lispy);

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
			lval *result = lval_eval(lval_read(r.output));
			lval_println(result);
			lval_del(result);
			mpc_ast_delete(r.output);
		} else {
			mpc_err_print(r.error);
			mpc_err_delete(r.error);
		}

		/* Free retrieved input */
		free(input);
	}

	/* Undefine and delete the parsers */
	mpc_cleanup(6, Number, Symbol, Sexpr, Qexpr, Expr, Lispy);

	return 0;
}

#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "mpc.h"

#define LASSERT(arg, cond, fmt, ...) \
	if (!(cond)) {\
		lval *err = lval_err(fmt, ##__VA_ARGS__);\
		lval_del(arg);\
		return err;\
	}

enum { LVAL_ERR, LVAL_NUM, LVAL_SYM,
       LVAL_FUN, LVAL_SEXPR, LVAL_QEXPR };
enum { LERR_DIV_ZERO, LERR_BAD_OP, LERR_BAD_NUM };

typedef struct lval lval;
typedef struct lenv lenv;
typedef lval * (*lbuiltin)(lenv *, lval *);


static lval * lval_err(char *fmt, ...);
static void lval_del(lval *v);
static lval * lval_eval(lenv *e, lval *v);
static lval * lval_eval_sexpr(lenv *e, lval *v);
static lval * lval_copy(lval *v);
static void lval_print(lval *v);
static lval * lval_take(lval *v, int i);
static lval * lval_pop(lval *v, int i);


struct lval {
	int type;

	long num;
	/* error and symbol types have string contents */
	char *err;
	char *sym;
	lbuiltin fun;

	/* count and pointer to a list of lval */
	int count;
	lval **cell;
};

struct lenv {
	int count;
	char **syms;
	lval **vals;
};


static char * ltype_name(int t) {
	switch(t) {
	case LVAL_FUN: return "Function";
	case LVAL_NUM: return "Number";
	case LVAL_ERR: return "Error";
	case LVAL_SYM: return "Symbol";
	case LVAL_SEXPR: return "S-Expression";
	case LVAL_QEXPR: return "Q-Expression";
	default: return "Unknown";
	}
}


static lenv * lenv_new(void) {
	lenv *e = malloc(sizeof(lenv));
	e->count = 0;
	e->syms = NULL;
	e->vals = NULL;
	return e;
}

static void lenv_del(lenv *e) {
	for (int i = 0; i < e->count; i++) {
		free(e->syms[i]);
		lval_del(e->vals[i]);
	}
	free(e->syms);
	free(e->vals);
	free(e);
}

static lval * lenv_get(lenv *e, lval *k) {
	for (int i = 0; i < e->count; i++) {
		if (strcmp(e->syms[i], k->sym) == 0) {
			return lval_copy(e->vals[i]);
		}
	}
	return lval_err("unbound symbol '%s'!", k->sym);
}

static void lenv_put(lenv *e, lval *k, lval *v) {
	/* See if variable already exists */
	for (int i = 0; i < e->count; i++) {
		if (strcmp(e->syms[i], k->sym) == 0) {
			lval_del(e->vals[i]);
			e->vals[i] = lval_copy(v);
			return;
		}
	}

	/* if no entry found allocate space for new entry */
	e->count++;
	e->vals = realloc(e->vals, sizeof(lval *) * e->count);
	e->syms = realloc(e->syms, sizeof(char *) * e->count);

	/* copy contents into new allocated space */
	e->vals[e->count - 1] = lval_copy(v);
	e->syms[e->count - 1] = malloc(strlen(k->sym) + 1);
	strcpy(e->syms[e->count - 1], k->sym);
}


static lval * lval_num(long x) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_NUM;
	v->num = x;
	return v;
}

static lval * lval_err(char *fmt, ...) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_ERR;

	/* create and initialize va_args */
	va_list va;
	va_start(va, fmt);

	/* allocate 512 byte for error */
	v->err = malloc(512);

	/* print maximal 511 byte into v->err */
	vsnprintf(v->err, 511, fmt, va);

	v->err = realloc(v->err, strlen(v->err) + 1);

	/* cleanup va_list */
	va_end(va);

	return v;
}

static lval * lval_sym(char *sym) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_SYM;
	v->sym = malloc(strlen(sym) + 1);
	strcpy(v->sym, sym);
	return v;
}

static lval * lval_fun(lbuiltin func) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_FUN;
	v->fun = func;
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
	/* no special handling for numbers or functions */
	case LVAL_NUM: break;
	case LVAL_FUN: break;
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


static lval * lval_eval(lenv *e, lval *v) {
	if (v->type == LVAL_SYM) {
		lval *x = lenv_get(e, v);
		lval_del(v);
		return x;
	}
	if (v->type == LVAL_SEXPR) {
		return lval_eval_sexpr(e, v);
	}
	return v;
}

static lval * lval_eval_sexpr(lenv *e, lval *v) {
	for (int i = 0; i < v->count; i++) {
		v->cell[i] = lval_eval(e, v->cell[i]);
	}

	for (int i = 0; i < v->count; i++) {
		if (v->cell[i]->type == LVAL_ERR) {
			return lval_take(v, i);
		}
	}

	if (v->count == 0) { return v; }
	if (v->count == 1) { return lval_take(v, 0); }

	/* Ensure first element is a function after evaluation */
	lval *f = lval_pop(v, 0);
	if (f->type != LVAL_FUN) {
		lval_del(v);
		lval_del(f);
		return lval_err("first element is not a function!");
	}

	/* call function to get result */
	lval *result = f->fun(e, v);
	lval_del(f);
	return result;
}


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
	case LVAL_FUN:
		printf("<function>");
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

static lval * lval_join(lval *x, lval *y) {

	/* for each cell in 'y' add it to 'x' */
	while (y->count) {
		x = lval_add(x, lval_pop(y, 0));
	}

	/* Delete empty 'y' and return 'x' */
	lval_del(y);
	return x;
}

static lval * lval_copy(lval *v) {
	lval *x = malloc(sizeof(lval));
	x->type = v->type;

	switch (v->type) {
		/* copy functions and numbers directly */
	case LVAL_NUM:
		x->num = v->num;
		break;
	case LVAL_FUN:
		x->fun = v->fun;
		break;

		/* strings need a deeper copy */
	case LVAL_ERR:
		x->err = malloc(strlen(v->err) + 1);
		strcpy(x->err, v->err);
		break;
	case LVAL_SYM:
		x->sym = malloc(strlen(v->sym) + 1);
		strcpy(x->sym, v->sym);
		break;

		/* copy lists by copying each element */
	case LVAL_SEXPR: /* no break! */
	case LVAL_QEXPR:
		x->count = v->count;
		x->cell = malloc(sizeof(lval *) * x->count);
		for (int i = 0; i < x->count; i++) {
			x->cell[i] = lval_copy(v->cell[i]);
		}
		break;
	}

	return x;
}


static lval * builtin_head(lenv *e, lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'head' passed too many arguments!"
		"Got %i, expected %i", a->count, 1);
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'head' passed incorrect type for argument 1!"
		"Got %s, expecpted %s",
		ltype_name(a->cell[0]->type), ltype_name(LVAL_QEXPR));
	LASSERT(a, a->cell[0]->count != 0,
		"Function 'head' passed {}!");

	lval *v = lval_take(a, 0);
	while(v->count > 1) {
		lval_del(lval_pop(v, 1));
	}
	return v;
}

static lval * builtin_tail(lenv *e, lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'tail' passed too many arguments!"
		"Got %i, expected %i", a->count, 1);
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'tail' passed incorrect type for argument 1!"
		"Got %s, expecpted %s",
		ltype_name(a->cell[0]->type), ltype_name(LVAL_QEXPR));
	LASSERT(a, a->cell[0]->count != 0,
		"Function 'tail' passed {}!");

	lval *v = lval_take(a, 0);
	lval_del(lval_pop(v, 0));
	return v;
}

static lval * builtin_list(lenv *e, lval *a) {
	a->type = LVAL_QEXPR;
	return a;
}

static lval * builtin_len(lenv *e, lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'head' passed too many arguments!"
		"Got %i, expected %i", a->count, 1);
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'head' passed incorrect type for argument 1!"
		"Got %s, expecpted %s",
		ltype_name(a->cell[0]->type), ltype_name(LVAL_QEXPR));

	lval *v = lval_num(a->cell[0]->count);
	lval_del(a);
	return v;
}

static lval * builtin_cons(lenv *e, lval *a) {
	LASSERT(a, a->count == 2,
		"Function 'cons' passed wrong number of arguments!"
		"Got %i, expected %i", a->count, 2);
	LASSERT(a, a->cell[0]->type == LVAL_NUM,
		"Function 'cons' passed incorrect type for argument 1!"
		"Got %s, expecpted %s",
		ltype_name(a->cell[0]->type), ltype_name(LVAL_NUM));
	LASSERT(a, a->cell[1]->type == LVAL_QEXPR,
		"Function 'cons' passed incorrect type for argument 2!"
		"Got %s, expecpted %s",
		ltype_name(a->cell[1]->type), ltype_name(LVAL_QEXPR));

	lval *n = lval_pop(a, 0);
	lval *v = lval_take(a, 0);

	v->count++;
	v->cell = realloc(v->cell, sizeof(lval *) * v->count);
	memmove(&v->cell[1], &v->cell[0], sizeof(lval *) * (v->count - 1));
	v->cell[0] = n;

	return v;
}

static lval * builtin_eval(lenv *e, lval *a) {
	LASSERT(a, a->count == 1,
		"Function 'eval' passed too many arguments!"
		"Got %i, expected %i", a->count, 1);
	LASSERT(a, a->cell[0]->type == LVAL_QEXPR,
		"Function 'eval' passed incorrect type for argument 1!"
		"Got %s, expecpted %s",
		ltype_name(a->cell[0]->type), ltype_name(LVAL_QEXPR));

	lval *x = lval_take(a, 0);
	x->type = LVAL_SEXPR;
	return lval_eval(e, x);
}

static lval * builtin_join(lenv *e, lval *a) {

	for (int i = 0; i < a->count; i++) {
		LASSERT(a, a->cell[i]->type == LVAL_QEXPR,
			"Function 'join' passed incorrect type for argument %i!"
			"Got %s, expecpted %s",
			i + 1, ltype_name(a->cell[i]->type), ltype_name(LVAL_QEXPR));
	}

	lval *x = lval_pop(a, 0);
	while (a->count) {
		x = lval_join(x, lval_pop(a, 0));
	}

	lval_del(a);
	return x;
}

static lval * builtin_op(lenv *e, lval *a, char *op) {
	
	/* Ensure all arguments are numbers */
	for (int i = 0; i < a->count; i++) {
		LASSERT(a, a->cell[i]->type == LVAL_NUM,
			"Function '%s' passed incorrect type for argument %i! "
			"Got %s, expected %s",
			op, i + 1, ltype_name(a->cell[i]->type), ltype_name(LVAL_NUM));
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

		if (strcmp(op, "+") == 0) { x->num += y->num; }
		if (strcmp(op, "-") == 0) { x->num -= y->num; }
		if (strcmp(op, "*") == 0) { x->num *= y->num; }
		if (strcmp(op, "/") == 0) {
			if (y->num == 0) {
				lval_del(x);
				lval_del(y);
				x = lval_err("Division by zero!");
				break;
			}
			x->num /= y->num;
		}
		if (strcmp(op, "%") == 0) {
			if (y->num == 0) {
				lval_del(x);
				lval_del(y);
				x = lval_err("Division by zero!");
				break;
			}
			x->num %= y->num;
		}
		if (strcmp(op, "^") == 0) { x->num = pow(x->num, y->num); }
		lval_del(y);
	}

	lval_del(a);
	return x;
}

static lval * builtin_add(lenv *e, lval *a) {
	return builtin_op(e, a, "+");
}

static lval * builtin_sub(lenv *e, lval *a) {
	return builtin_op(e, a, "-");
}

static lval * builtin_mul(lenv *e, lval *a) {
	return builtin_op(e, a, "*");
}

static lval * builtin_div(lenv *e, lval *a) {
	return builtin_op(e, a, "/");
}


static void lenv_add_builtin(lenv *e, char *name, lbuiltin func) {
	lval *k = lval_sym(name);
	lval *v = lval_fun(func);
	lenv_put(e, k, v);
	lval_del(k);
	lval_del(v);
}

static void lenv_add_builtins(lenv *e) {
	/* list functions */
	lenv_add_builtin(e, "list", builtin_list);
	lenv_add_builtin(e, "head", builtin_head);
	lenv_add_builtin(e, "tail", builtin_tail);
	lenv_add_builtin(e, "eval", builtin_eval);
	lenv_add_builtin(e, "join", builtin_join);
	lenv_add_builtin(e, "cons", builtin_cons);
	lenv_add_builtin(e, "len", builtin_len);

	/* mathematical functions */
	lenv_add_builtin(e, "+", builtin_add);
	lenv_add_builtin(e, "-", builtin_sub);
	lenv_add_builtin(e, "*", builtin_mul);
	lenv_add_builtin(e, "/", builtin_div);
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
		  "symbol   : /[a-zA-Z0-9_+\\-*\\/\\\\=<>!&]+/ ;"
		  "sexpr    : '(' <expr>* ')' ;"
		  "qexpr    : '{' <expr>* '}' ;"
		  "expr     : <number> | <symbol> | <sexpr> | <qexpr> ;"
		  "lispy    : /^/ <expr>* /$/ ; "
		  "",
		  Number, Symbol, Sexpr, Qexpr, Expr, Lispy);

	lenv *e = lenv_new();
	lenv_add_builtins(e);

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
			lval *result = lval_eval(e, lval_read(r.output));
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

	lenv_del(e);

	/* Undefine and delete the parsers */
	mpc_cleanup(6, Number, Symbol, Sexpr, Qexpr, Expr, Lispy);

	return 0;
}

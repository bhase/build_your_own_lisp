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

#define LASSERT_TYPE_AT(arg, pos, t, fn) \
	LASSERT(arg, arg->cell[pos]->type == t,\
		"Function '%s' passed incorrect type for argument %i! "\
		"Got %s, expected %s",\
		fn, pos + 1, ltype_name(arg->cell[pos]->type), ltype_name(t))

#define LASSERT_NUM_AT(arg, pos, fn) \
	LASSERT_TYPE_AT(arg, pos, LVAL_NUM, fn)

#define LASSERT_QEXPR_AT(arg, pos, fn) \
	LASSERT_TYPE_AT(arg, pos, LVAL_QEXPR, fn)

#define LASSERT_BOOL_AT(arg, pos, fn) \
	LASSERT_TYPE_AT(arg, pos, LVAL_BOOL, fn)

#define LASSERT_COUNT(arg, num, fn) \
	LASSERT(arg, arg->count == num, \
		"Function '%s' passed incorrect number of arguments! "\
		"Got %i, expected %i", fn, arg->count, num)

enum { LVAL_ERR, LVAL_NUM,   LVAL_SYM,  LVAL_BOOL,
       LVAL_FUN, LVAL_SEXPR, LVAL_QEXPR };
enum { LERR_DIV_ZERO, LERR_BAD_OP, LERR_BAD_NUM };

typedef struct lval lval;
typedef struct lenv lenv;
typedef lval * (*lbuiltin)(lenv *, lval *);

typedef unsigned int boolean;

static lval * lval_err(char *fmt, ...);
static void lval_del(lval *v);
static lval * lval_eval(lenv *e, lval *v);
static lval * lval_eval_sexpr(lenv *e, lval *v);
static lval * lval_copy(lval *v);
static void lval_print(lval *v);
static lval * lval_take(lval *v, int i);
static lval * lval_pop(lval *v, int i);
static lval * lval_call(lenv *e, lval *f, lval *a);
static lval * builtin_eval(lenv *e, lval *a);
static lval * builtin_list(lenv *e, lval *a);


struct lval {
	int type;

	/* Basic */
	long num;
	boolean b;
	char *err;
	char *sym;

	/* Function */
	lbuiltin builtin;
	lenv *env;
	lval *formals;
	lval *body;

	/* Expression */
	int count;
	lval **cell;
};

struct lenv {
	lenv *par;
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
	case LVAL_BOOL: return "Boolean";
	default: return "Unknown";
	}
}


static lenv * lenv_new(void) {
	lenv *e = malloc(sizeof(lenv));
	e->par = NULL;
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

	if (e->par) {
		return lenv_get(e->par, k);
	} else {
		return lval_err("unbound symbol '%s'!", k->sym);
	}
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

static lenv * lenv_copy(lenv *e) {
	lenv *n = malloc(sizeof(lenv));
	n->par = e->par;
	n->count = e->count;
	n->syms = malloc(sizeof(char *) * n->count);
	n->vals = malloc(sizeof(lval *) * n->count);
	for (int i = 0; i < n->count; i++) {
		n->syms[i] = malloc(strlen(e->syms[i]) + 1);
		strcpy(n->syms[i], e->syms[i]);
		n->vals[i] = lval_copy(e->vals[i]);
	}
	return n;
}

static void lenv_def(lenv *e, lval *k, lval *v) {
	/* iterate until no parent */
	while (e->par) { e = e->par; }
	lenv_put(e, k, v);
}


static lval * lval_num(long x) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_NUM;
	v->num = x;
	return v;
}

static lval * lval_bool(boolean x) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_BOOL;
	v->b = x;
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
	v->builtin = func;
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

static lval * lval_lambda(lval *formals, lval *body) {
	lval *v = malloc(sizeof(lval));
	v->type = LVAL_FUN;

	v->builtin = NULL;

	/* Build new environment */
	v->env = lenv_new();

	/* Set formals and body */
	v->formals = formals;
	v->body = body;
	return v;
}


static void lval_del(lval *v) {
	switch (v->type) {
	/* no special handling for numbers or functions */
	case LVAL_NUM: break;
	case LVAL_BOOL: break;
	case LVAL_FUN:
		      if (v->builtin == NULL) {
			      lenv_del(v->env);
			      lval_del(v->formals);
			      lval_del(v->body);
		      }
		      break;
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
		lval *err = lval_err(
		    "S-Expression starts with incorrect type. "
		    "Got %s, expected %s.",
		    ltype_name(f->type), ltype_name(LVAL_FUN));
		lval_del(v);
		lval_del(f);
		return err;
	}

	lval *result = lval_call(e, f, v);
	lval_del(f);
	return result;
}

static lval * lval_call(lenv *e, lval *f, lval *a) {

	/* If builtin then simply call that */
	if (f->builtin) {
		return f->builtin(e, a);
	}

	/* Record argument count */
	int given = a->count;
	int total = f->formals->count;

	/* While there are arguments to be processed */
	while(a->count) {

		/* If ran out of formal arguments to bind */
		if (f->formals->count == 0) {
			lval_del(a);
			return lval_err("Function passed too many arguments. "
					"Got %i, expected %i.", given, total);
		}

		/* Pop the first symbol from formals */
		lval *sym = lval_pop(f->formals, 0);

		if (strcmp(sym->sym, "&") == 0) {
			/* Ensure '&' is followed by another symbol */
			if (f->formals->count != 1) {
				lval_del(a);
				return lval_err("Function format invalid. "
						"Symbol '&' not followed by a single symbol.");
			}

			/* Next formal should be bound to remaining
			 * arguments */
			lval *nsym = lval_pop(f->formals, 0);
			lenv_put(f->env, nsym, builtin_list(e, a));
			lval_del(sym);
			lval_del(nsym);
			break;
		}

		/* Pop the next argument from list */
		lval *val = lval_pop(a, 0);

		/* Bind a copy into the functions environment */
		lenv_put(f->env, sym, val);

		lval_del(sym);
		lval_del(val);
	}

	/* Argument list is now bound so it can be cleaned up */
	lval_del(a);

	/* If '&' remains in formal list bind to empty list */
	if (    f->formals->count > 0
	    && strcmp(f->formals->cell[0]->sym, "&") == 0) {

		/* Check to ensure that it is not passed invalidly */
		if (f->formals->count != 2) {
			return lval_err("Function format invalid. "
					"Symbol '&' not followed by a single symbol.");
		}

		/* Pop and delete '&' symbol */
		lval_del(lval_pop(f->formals, 0));

		/* Pop next symbol and create empty list */
		lval *sym = lval_pop(f->formals, 0);
		lval *val = lval_qexpr();

		/* bind to environment and delete */
		lenv_put(f->env, sym, val);
		lval_del(sym);
		lval_del(val);
	}

	/* If all formals have been bound: evaluate */
	if (f->formals->count == 0) {
		/* Set environment parent */
		f->env->par = e;

		/* Evaluate */
		return builtin_eval(f->env,
				    lval_add(lval_sexpr(), lval_copy(f->body)));
	} else {
		/* otherwise return partially evaluated function */
		return lval_copy(f);
	}
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
		if (v->builtin) {
			printf("<function>");
		} else {
			printf("(\\ ");
			lval_print(v->formals);
			putchar(' ');
			lval_print(v->body);
			putchar(')');
		}
		break;
	case LVAL_SEXPR:
		lval_expr_print(v, '(', ')');
		break;
	case LVAL_QEXPR:
		lval_expr_print(v, '{', '}');
		break;
	case LVAL_BOOL:
		if (v->b) {
			printf("t");
		} else {
			printf("false");
		}
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
	case LVAL_BOOL:
		x->b = v->b;
		break;
	case LVAL_FUN:
		if (v->builtin) {
			x->builtin = v->builtin;
		} else {
			x->builtin = NULL;
			x->env = lenv_copy(v->env);
			x->formals = lval_copy(v->formals);
			x->body = lval_copy(v->body);
		}
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


int lval_eq(lval *x, lval *y) {

	/* Different types? Always unequal. */
	if (x->type != y->type) {
		return 0;
	}

	switch (x->type) {
	case LVAL_NUM:
		return x->num == y->num;
	case LVAL_BOOL:
		return x->b == y->b;
	case LVAL_ERR:
		return (strcmp(x->err, y->err) == 0);
	case LVAL_SYM:
		return (strcmp(x->sym, y->sym) == 0);

	case LVAL_FUN:
		/* If builtin compare pointer otherwise
		 * compare formals and body */
		if (x->builtin || y->builtin) {
			return x->builtin == y->builtin;
		} else {
			return     lval_eq(x->formals, y->formals)
				&& lval_eq(x->body, y->body);
		}

	case LVAL_QEXPR:
	case LVAL_SEXPR:
		/* for lists compare each element individually */
		if (x->count != y->count) { return 0; }
		for (int i = 0; i < x->count; i++) {
			if (!lval_eq(x->cell[i], y->cell[i])) {
				/* one element differs -> list not equal */
				return 0;
			}
		}
		return 1;
	}
	return 0;
}


static lval * builtin_op(lenv *e, lval *a, char *op) {

	/* Ensure all arguments are numbers */
	for (int i = 0; i < a->count; i++) {
		LASSERT_NUM_AT(a, i, op);
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

static lval * builtin_ord(lenv *e, lval *a, char *op) {
	LASSERT_COUNT(a, 2, op);
	LASSERT_NUM_AT(a, 0, op);
	LASSERT_NUM_AT(a, 1, op);

	int r;
	if (strcmp(op, ">") == 0) {
		r = (a->cell[0]->num > a->cell[1]->num);
	}
	if (strcmp(op, "<") == 0) {
		r = (a->cell[0]->num < a->cell[1]->num);
	}
	if (strcmp(op, ">=") == 0) {
		r = (a->cell[0]->num >= a->cell[1]->num);
	}
	if (strcmp(op, "<=") == 0) {
		r = (a->cell[0]->num <= a->cell[1]->num);
	}
	lval_del(a);
	return lval_bool(r);
}


static lval * builtin_head(lenv *e, lval *a) {
	LASSERT_COUNT(a, 1, "head");
	LASSERT_QEXPR_AT(a, 0, "head");
	LASSERT(a, a->cell[0]->count != 0,
		"Function 'head' passed {}!");

	lval *v = lval_take(a, 0);
	while(v->count > 1) {
		lval_del(lval_pop(v, 1));
	}
	return v;
}

static lval * builtin_tail(lenv *e, lval *a) {
	LASSERT_COUNT(a, 1, "tail");
	LASSERT_QEXPR_AT(a, 0, "tail");
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
	LASSERT_COUNT(a, 1, "head");
	LASSERT_QEXPR_AT(a, 0, "head");

	lval *v = lval_num(a->cell[0]->count);
	lval_del(a);
	return v;
}

static lval * builtin_cons(lenv *e, lval *a) {
	LASSERT_COUNT(a, 2, "cons");
	LASSERT_NUM_AT(a, 0, "cons");
	LASSERT_QEXPR_AT(a, 1, "cons");

	lval *n = lval_pop(a, 0);
	lval *v = lval_take(a, 0);

	v->count++;
	v->cell = realloc(v->cell, sizeof(lval *) * v->count);
	memmove(&v->cell[1], &v->cell[0], sizeof(lval *) * (v->count - 1));
	v->cell[0] = n;

	return v;
}

static lval * builtin_eval(lenv *e, lval *a) {
	LASSERT_COUNT(a, 1, "eval");
	LASSERT_QEXPR_AT(a, 0, "eval");

	lval *x = lval_take(a, 0);
	x->type = LVAL_SEXPR;
	return lval_eval(e, x);
}

static lval * builtin_join(lenv *e, lval *a) {

	for (int i = 0; i < a->count; i++) {
		LASSERT_QEXPR_AT(a, i, "join");
	}

	lval *x = lval_pop(a, 0);
	while (a->count) {
		x = lval_join(x, lval_pop(a, 0));
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

static lval * builtin_var(lenv *e, lval *a, char *func) {
	LASSERT_QEXPR_AT(a, 0, func);

	lval *syms = a->cell[0];
	for (int i = 0; i < syms->count; i++) {
		LASSERT(a, syms->cell[i]->type == LVAL_SYM,
			"Function '%s' cannot define non-symbol. Got %s, expected %s.",
			func, ltype_name(syms->cell[i]->type), ltype_name(LVAL_SYM));
	}
	LASSERT(a, syms->count == a->count - 1,
		"Function '%s' passed too many arguments for symbols. "
		"Got %i, expected %i.", func, syms->count, a->count - 1);

	for (int i = 0; i < syms->count; i++) {
		/* If "def" define in globally. If "put" define locally. */
		if (strcmp(func, "def") == 0) {
			lenv_def(e, syms->cell[i], a->cell[i + 1]);
		}

		if (strcmp(func, "=") == 0) {
			lenv_put(e, syms->cell[i], a->cell[i + 1]);
		}
	}

	lval_del(a);
	return lval_sexpr();
}

static lval * builtin_lambda(lenv *e, lval *a) {
	/* Check two arguments, each of which are Q-Expressions */
	LASSERT_COUNT(a, 2, "\\");
	LASSERT_QEXPR_AT(a, 0, "\\");
	LASSERT_QEXPR_AT(a, 1, "\\");

	/* Check if first Q-Expression contains only symbols */
	for (int i = 0; i < a->cell[0]->count; i++) {
		LASSERT(a, a->cell[0]->cell[i]->type == LVAL_SYM,
			"Cannot define non-symbol. Got %s, expected %s.",
			ltype_name(a->cell[0]->cell[i]->type), ltype_name(LVAL_SYM));
	}

	/* Pop first two elements and pass them to lval_lambda */
	lval *formals = lval_pop(a, 0);
	lval *body = lval_pop(a, 0);
	lval_del(a);

	return lval_lambda(formals, body);
}

static lval * builtin_def(lenv *e, lval *a) {
	return builtin_var(e, a, "def");
}

static lval * builtin_put(lenv *e, lval *a) {
	return builtin_var(e, a, "=");
}

static lval * builtin_gt(lenv *e, lval *a) {
	return builtin_ord(e, a, ">");
}

static lval * builtin_lt(lenv *e, lval *a) {
	return builtin_ord(e, a, "<");
}

static lval * builtin_ge(lenv *e, lval *a) {
	return builtin_ord(e, a, ">=");
}

static lval * builtin_le(lenv *e, lval *a) {
	return builtin_ord(e, a, "<=");
}

static lval * builtin_cmp(lenv *e, lval *a, char *op) {
	LASSERT_COUNT(a, 2, op);

	int r;
	if (strcmp(op, "==") == 0) {
		r = lval_eq(a->cell[0], a->cell[1]);
	}
	if (strcmp(op, "!=") == 0) {
		r = !lval_eq(a->cell[0], a->cell[1]);
	}
	lval_del(a);
	return lval_bool(r);
}

static lval * builtin_eq(lenv *e, lval *a) {
	return builtin_cmp(e, a, "==");
}

static lval * builtin_ne(lenv *e, lval *a) {
	return builtin_cmp(e, a, "!=");
}

static lval * builtin_if(lenv *e, lval *a) {
	LASSERT_COUNT(a, 3, "if");
	LASSERT_BOOL_AT(a, 0, "if");
	LASSERT_QEXPR_AT(a, 1, "if");
	LASSERT_QEXPR_AT(a, 2, "if");

	/* mark both expressions as evaluable */
	lval *x;
	a->cell[1]->type = LVAL_SEXPR;
	a->cell[2]->type = LVAL_SEXPR;

	if (a->cell[0]->b) {
		x = lval_eval(e, lval_pop(a, 1));
	} else {
		x = lval_eval(e, lval_pop(a, 2));
	}

	lval_del(a);
	return x;
}

static lval * builtin_or(lenv *e, lval *a) {
	LASSERT_COUNT(a, 2, "||");
	LASSERT_BOOL_AT(a, 0, "||");
	LASSERT_BOOL_AT(a, 1, "||");

	lval *x;
	if (a->cell[0]->b || a->cell[1]->b) {
		x = lval_bool(1);
	} else {
		x = lval_bool(0);
	}

	lval_del(a);
	return x;
}

static lval * builtin_and(lenv *e, lval *a) {
	LASSERT_COUNT(a, 2, "&&");
	LASSERT_BOOL_AT(a, 0, "&&");
	LASSERT_BOOL_AT(a, 1, "&&");

	lval *x;
	if (a->cell[0]->b && a->cell[1]->b) {
		x = lval_bool(1);
	} else {
		x = lval_bool(0);
	}

	lval_del(a);
	return x;
}

static lval * builtin_not(lenv *e, lval *a) {
	LASSERT_COUNT(a, 1, "!");
	LASSERT_NUM_AT(a, 0, "!");

	lval *x;
	if (a->cell[0]->b) {
		/* Invert it */
		x = lval_bool(0);
	} else {
		x = lval_bool(1);
	}

	lval_del(a);
	return x;
}

static void lenv_add_builtin(lenv *e, char *name, lbuiltin func) {
	lval *k = lval_sym(name);
	lval *v = lval_fun(func);
	lenv_put(e, k, v);
	lval_del(k);
	lval_del(v);
}

static void lenv_add_builtin_bool(lenv *e, char *sym, boolean val) {
	lval *v = lval_bool(val);
	lval *k = lval_sym(sym);
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

	lenv_add_builtin(e, "def", builtin_def);
	lenv_add_builtin(e, "=", builtin_put);
	lenv_add_builtin(e, "\\", builtin_lambda);

	lenv_add_builtin(e, "if", builtin_if);
	lenv_add_builtin(e, "==", builtin_eq);
	lenv_add_builtin(e, "!=", builtin_ne);
	lenv_add_builtin(e, ">",  builtin_gt);
	lenv_add_builtin(e, "<",  builtin_lt);
	lenv_add_builtin(e, ">=", builtin_ge);
	lenv_add_builtin(e, "<=", builtin_le);

	lenv_add_builtin(e, "||", builtin_or);
	lenv_add_builtin(e, "&&", builtin_and);
	lenv_add_builtin(e, "!",  builtin_not);

	lenv_add_builtin_bool(e, "t", 1);
	lenv_add_builtin_bool(e, "false", 0);
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
		  "symbol   : /[a-zA-Z0-9_+\\-*\\/\\\\=<>!&|]+/ ;"
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

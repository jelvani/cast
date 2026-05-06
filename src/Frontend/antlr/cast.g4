grammar cast;

startRule:
	spaceDecl? decl_package? importsRule? (
		consts
		| decl_enum
		| decl_type
		| decl_func
		| decl_machine
		| decl_instantiate
		| decl_assertions
	)* EOF;

spaceDecl: 'space' ident;

importsRule: 'import' single_import | 'import' multiple_imports;
single_import: '('? CNAME ')'?;
multiple_imports: '(' CNAME+ ')';

consts: stmt+;

decl_compile_time_params:
	'[' (ident_typed_param (',' ident_typed_param)*)? ']';
ident_typed_param: ident ':' type;

decl_enum: 'enum' ident '=' '{' (ident (',' ident)*) '}' ';';
decl_package: 'package' ident;
decl_instantiate: 'instantiate' stmt_block;
decl_assertions: 'assertions' stmt_block;
decl_machine:
	'machine' func_name decl_compile_time_params? machine_block;
decl_interface: 'interface' interface_block;
decl_memory: 'memory' memory_block;
decl_shared: 'shared' shared_block;
decl_states: 'states' states_block ';'?;
decl_state:
	ident ':' (stmt_binary (',' stmt_binary)*)? stmt_block;
decl_exception:
	'exception' ident ':' (stmt_binary (',' stmt_binary)*)? stmt_block;
decl_type: 'type' ident type;
decl_func:
	'func' func_name func_args type_func_return? stmt_block;
decl_io: direction=(INPUT | OUTPUT) (ident_typed | ident) (
		ident (',' ident)*
	) ';'?;

machine_block:
	'{' decl_interface decl_shared? decl_memory? decl_states? '}';
memory_block: '{' (ident_typed (',' ident)* ';'?)* '}';
shared_block: '{' decl_var* '}';
states_block: '{' (decl_state | decl_exception)* '}';

interface_block: '{' decl_io* '}';
stmt_block: '{' stmt* '}';

stmt: (
		decl_var
		| inst_module
		| stmt_for
		| stmt_if
		| stmt_switch
		| stmt_binary
		| stmt_return
		| stmt_block
		| expr
		| constDecl
		| stmt_nextstate
	) ';'?;

stmt_nextstate: 'goto' ident;

expr_multiple: expr (',' expr)*;
stmt_return: 'return' expr_multiple;
stmt_binary: expr assignment_op expr;
stmt_update: expr_multiple update_op;
stmt_switch: 'switch' stmt '{' expr_switch_case* '}';
expr_switch_case:
	'case' type ':' stmt*
	| 'case' expr ':' stmt*
	| 'default' ':' stmt*;

decl_var: ('var' ident_typed ('=' expr)?) ';'?;

inst_module: ident '=' expr_func_call ';'?;

constDecl: 'const' CNAME type? '=' expr;

stmt_for:
	'for' '(' (
		(stmt_binary | decl_var) ';' expr ';' (
			stmt_binary
			| stmt_update
		)
	) ')' stmt_block;
stmt_if: 'if' expr stmt_block ('else' (stmt_block | stmt_if))?;

expr:
	ident
	| ident_field
	| string_literal
	| number_literal
	| nil_literal
	| '(' expr ')'
	| unary_op expr
	| expr '[' (expr | expr? ':' expr?) ']'
	| expr '(' (expr (',' expr)*)? ')' expr_compile_time_params?
	| expr bin_op expr
	| expr update_op
	| expr_func_call;

expr_compile_time_params:
	'[' ((ident '=')? expr (',' (ident '=')? expr)*)? ']';

bin_op:
	'+'
	| '-'
	| '*'
	| '/'
	| '&&'
	| '||'
	| '!='
	| '=='
	| '>='
	| '<='
	| '>'
	| '<'
	| '%'
	| '<<'
	| '>>'
	| '|'
	| '&'
	| '^';
unary_op: '!' | '&' | '*';
assignment_op:
	ASSIGN_CHANNEL_RECEIVE
	| ASSIGN_CHANNEL_SEND
	| ASSIGN_WALRUS
	| ASSIGN
	| ASSIGN_ADD
	| ASSIGN_SUB
	| ASSIGN_MUL
	| ASSIGN_DIV
	| ASSIGN_XOR
	| ASSIGN_SHL
	| ASSIGN_SHR;
update_op: '++' | '--';

ident_typed: type (ident (',' ident)*)? | ident (',' ident)*;

type: type_inline | type_chan | type_lit;

type_inline: type_slice | type_array | type_map;

ident_scoped: ident '.' ident;
type_slice: '[' ']' type;
type_array: '[' INTEGER ']' type;
type_map: 'map' '[' type ']' type;
type_chan: 'chan' type | 'chan' '<-' type | 'chan' '->' type;

func_name: ident;
func_args: '(' (func_arg (',' func_arg)*)? ')';
func_arg: ident_typed;
type_func_return: type;

type_lit:
	T_STRING
	| T_INTEGER
	| T_FLOAT
	| T_BYTE
	| T_INT32
	| T_UINT32
	| T_UINT16
	| T_BOOL;
ident: CNAME;
number_literal: HEX | INTEGER;
string_literal: ESCAPED_STRING;
nil_literal: NULL;

ident_field: ident '.' ident;
expr_indexed: expr '[' (expr | expr? ':' expr?) ']';
expr_binary: expr bin_op expr;
expr_unary: unary_op expr;
expr_func_call:
	func_name expr_compile_time_params? '(' (expr (',' expr)*)? ')';

// lexer rules - order matters

CPP_LINE_COMMENT: '//' ~[\r\n]* -> skip;
CPP_BLOCK_COMMENT: '/*' .*? '*/' -> skip;

WS: [ \t\r\n]+ -> skip;
ESCAPED_STRING: '"' ('\\"' | ~["])*? '"';
INTEGER: [0-9]+;
FLOAT: [0-9]+ ('.' [0-9]+)?;
HEX: '0x' [0-9a-fA-F]+;
NULL: 'nil';

// assignment operators
ASSIGN_CHANNEL_RECEIVE: '<-';
ASSIGN_CHANNEL_SEND: '->';
ASSIGN_WALRUS: ':=';
ASSIGN: '=';
ASSIGN_ADD: '+=';
ASSIGN_SUB: '-=';
ASSIGN_MUL: '*=';
ASSIGN_DIV: '/=';
ASSIGN_XOR: '^=';
ASSIGN_SHL: '<<=';
ASSIGN_SHR: '>>=';

// types
T_STRING: 'string';
T_INTEGER: 'int';
T_FLOAT: 'float';
T_BYTE: 'byte';
T_INT32: 'int32';
T_UINT32: 'uint32';
T_UINT16: 'uint16';
T_BOOL: 'bool';

INPUT: 'input' ;
OUTPUT: 'output' ;

CNAME: [a-zA-Z_][a-zA-Z0-9_]*;
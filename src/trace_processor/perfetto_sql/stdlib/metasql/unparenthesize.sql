CREATE PERFETTO MACRO __unparenthesize_identity(
    x ColumnName
)
RETURNS Expr AS
$x;

-- Removes parentheses from an expression. Typically used when you need to
-- wrap parentheses around e.g. a column list.
CREATE PERFETTO MACRO metasql_unparenthesize(
    -- Argument to unparenthesize.
    expr Expr
)
RETURNS Expr AS
__intrinsic_token_apply!(__unparenthesize_identity, $expr);

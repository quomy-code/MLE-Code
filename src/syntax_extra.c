#include "mle.h"

/* Extra syntax packs. Registered after syn_generic, so a matching pack is
 * tried first. Patterns stay single-line except for a few block comments.
 * Tree-sitter is more accurate and much larger; these PCRE rules keep mle fast. */

#define FG_STR (TB_YELLOW | TB_BOLD)
#define FG_CMT TB_CYAN
#define FG_KW  TB_GREEN
#define FG_NUM (TB_BLUE | TB_BOLD)
#define FG_PUN (TB_RED | TB_BOLD)
#define FG_PRE (TB_MAGENTA | TB_BOLD)
#define S(re, fg) { (re), NULL, (fg), TB_DEFAULT }
#define MM(a, b, fg) { (a), (b), (fg), TB_DEFAULT }
#define SYN_END { NULL, NULL, 0, 0 }

static void add(editor_t *editor, char *name, char *path, srule_def_t *defs) {
    editor_add_syntax(editor, name, path, defs);
}

void syntax_extra_init(editor_t *editor) {
    add(editor, "syn_c", "\\.(c|h|cc|cpp|cxx|hh|hpp|hxx|ipp)$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("//.*$", FG_CMT),
        S("^\\s*#.*$", FG_PRE),
        S("\\b(auto|break|case|char|const|continue|default|do|double|else|enum|extern|float|for|goto|if|inline|int|long|register|return|short|signed|sizeof|static|struct|switch|typedef|union|unsigned|void|volatile|while|bool|class|namespace|template|typename|virtual|public|private|protected|new|delete|this|nullptr|true|false|try|catch|throw|using|constexpr|consteval|constinit|concept|requires|co_await|co_return|co_yield|noexcept|override|final|thread_local|static_assert)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_python", "\\.(py|pyi|pyw)$", (srule_def_t[]){
        MM("\"\"\"", "\"\"\"", FG_STR),
        MM("'''", "'''", FG_STR),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("#.*$", FG_CMT),
        S("@[A-Za-z_][A-Za-z0-9_.]*", FG_PRE),
        S("\\b(and|as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield|match|case|True|False|None)\\b", FG_KW),
        S("\\b(self|cls)\\b", FG_PRE),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_js", "\\.(js|mjs|cjs|jsx|ts|tsx|mts|cts)$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("`[^`]*`", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(async|await|break|case|catch|class|const|continue|debugger|default|delete|do|else|export|extends|false|finally|for|from|function|if|import|in|instanceof|let|new|null|return|satisfies|static|super|switch|this|throw|true|try|typeof|undefined|var|void|while|yield|interface|type|enum|implements|keyof|infer|namespace|declare|abstract|public|private|protected|readonly|as|package)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_rust", "\\.rs$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(as|async|await|break|const|continue|crate|dyn|else|enum|extern|false|fn|for|if|impl|in|let|loop|match|mod|move|mut|pub|ref|return|self|Self|static|struct|super|trait|true|type|unsafe|use|where|while)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_go", "\\.go$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("`[^`]*`", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(break|case|chan|const|continue|default|defer|else|fallthrough|for|func|go|goto|if|import|interface|map|package|range|return|select|struct|switch|type|var|true|false|nil)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_java", "\\.java$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("@[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b(abstract|assert|boolean|break|byte|case|catch|char|class|const|continue|default|do|double|else|enum|extends|final|finally|float|for|goto|if|implements|import|instanceof|int|interface|long|native|new|package|private|protected|public|return|short|static|strictfp|super|switch|synchronized|this|throw|throws|transient|try|void|volatile|while|true|false|null|var|record|sealed|permits)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_cs", "\\.cs$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(abstract|as|base|bool|break|byte|case|catch|char|checked|class|const|continue|decimal|default|delegate|do|double|else|enum|event|explicit|extern|false|finally|fixed|float|for|foreach|goto|if|implicit|in|int|interface|internal|is|lock|long|namespace|new|null|object|operator|out|override|params|private|protected|public|readonly|ref|return|sbyte|sealed|short|sizeof|stackalloc|static|string|struct|switch|this|throw|true|try|typeof|uint|ulong|unchecked|unsafe|ushort|using|virtual|void|volatile|while|var|async|await)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_kt", "\\.kts?$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(as|break|class|continue|do|else|false|for|fun|if|in|interface|is|null|object|package|return|super|this|throw|true|try|typealias|typeof|val|var|when|while|import|by|catch|constructor|delegate|dynamic|field|file|finally|get|init|param|property|receiver|set|setparam|where|actual|abstract|annotation|companion|crossinline|data|enum|expect|external|final|infix|inline|inner|internal|lateinit|noinline|open|operator|out|override|private|protected|public|reified|sealed|suspend|tailrec|vararg)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_swift", "\\.swift$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(associatedtype|class|deinit|enum|extension|fileprivate|func|import|init|inout|internal|let|open|operator|private|protocol|public|rethrows|static|struct|subscript|typealias|var|break|case|continue|default|defer|do|else|fallthrough|for|guard|if|in|repeat|return|switch|where|while|as|catch|false|is|nil|super|self|Self|throw|throws|true|try|async|await)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_php", "\\.php$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("//.*$", FG_CMT),
        S("#.*$", FG_CMT),
        S("\\$[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b(function|class|interface|trait|namespace|use|public|private|protected|static|final|abstract|extends|implements|new|return|if|else|elseif|endif|for|foreach|while|do|switch|case|break|continue|echo|print|require|include|require_once|include_once|true|false|null|as|and|or|xor|try|catch|finally|throw|const|var|global|isset|unset|empty)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_ruby", "\\.(rb|erb|rake)$", (srule_def_t[]){
        MM("=begin", "=end", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("#.*$", FG_CMT),
        S(":[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("@[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b(alias|and|begin|break|case|class|def|defined|do|else|elsif|end|ensure|false|for|if|in|module|next|nil|not|or|redo|rescue|retry|return|self|super|then|true|undef|unless|until|when|while|yield|require|include)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_lua", "\\.lua$", (srule_def_t[]){
        MM("--\\[\\[", "\\]\\]", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("--.*$", FG_CMT),
        S("\\b(and|break|do|else|elseif|end|false|for|function|goto|if|in|local|nil|not|or|repeat|return|then|true|until|while)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_zig", "\\.zig$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(addrspace|align|allowzero|and|anyframe|anytype|asm|async|await|break|callconv|catch|comptime|const|continue|defer|else|enum|errdefer|error|export|extern|fn|for|if|inline|noalias|noinline|nosuspend|opaque|or|orelse|packed|pub|resume|return|linksection|struct|suspend|switch|test|threadlocal|try|union|unreachable|usingnamespace|var|volatile|while|true|false|null|undefined)\\b", FG_KW),
        S("\\b(-?(0x)?[0-9]+(\\.[0-9]+)?)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_sh", "\\.(sh|bash|zsh|fish)$|(^|/)\\.bashrc$|(^|/)\\.zshrc$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'[^']*'", FG_STR),
        S("#.*$", FG_CMT),
        S("\\$\\{[^}]+\\}", FG_PRE),
        S("\\$[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b(if|then|else|elif|fi|for|while|until|do|done|case|esac|in|function|return|local|export|readonly|exit|echo|source|select|time|coproc)\\b", FG_KW),
        SYN_END
    });
    add(editor, "syn_html", "\\.(html|htm|xml|svg|xsl|vue|svelte|xhtml)$", (srule_def_t[]){
        MM("<!--", "-->", FG_CMT),
        S("\"[^\"]*\"", FG_STR),
        S("'[^']*'", FG_STR),
        S("</?[A-Za-z!][^>]*>", FG_KW),
        SYN_END
    });
    add(editor, "syn_css", "\\.(css|scss|sass|less)$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("//.*$", FG_CMT),
        S("#[0-9A-Fa-f]{3,8}\\b", FG_NUM),
        S("[.#@][A-Za-z_-][A-Za-z0-9_-]*", FG_PRE),
        S("\\b[0-9]+(\\.[0-9]+)?(px|em|rem|vh|vw|%|s|ms|deg)?\\b", FG_NUM),
        S("\\b(color|background|margin|padding|border|display|flex|grid|position|width|height|font|content|none|auto|absolute|relative|fixed|block|inline|important)\\b", FG_KW),
        SYN_END
    });
    add(editor, "syn_json", "\\.(json|jsonc|webmanifest)$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("//.*$", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"\\s*:", FG_KW),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?", FG_NUM),
        S("\\b(true|false|null)\\b", FG_PRE),
        SYN_END
    });
    add(editor, "syn_yaml", "\\.(ya?ml)$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("^\\s*[A-Za-z0-9_.-]+\\s*:", FG_KW),
        S("\\b(true|false|null|yes|no|on|off)\\b", FG_PRE),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_toml", "\\.toml$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        MM("\"\"\"", "\"\"\"", FG_STR),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'[^']*'", FG_STR),
        S("^\\s*\\[[^\\]]+\\]", FG_PRE),
        S("^\\s*[A-Za-z0-9_.-]+\\s*=", FG_KW),
        S("\\b(true|false)\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_md", "\\.(md|markdown|mdown)$|(^|/)README$", (srule_def_t[]){
        S("^```.*$", FG_CMT),
        S("^#{1,6}.*$", FG_KW | TB_BOLD),
        S("`[^`]+`", FG_STR),
        S("\\[[^\\]]+\\]\\([^)]+\\)", FG_PRE),
        S("^\\s*[-*+]\\s", FG_PUN),
        S("^>.*$", FG_CMT),
        SYN_END
    });
    add(editor, "syn_sql", "\\.sql$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("--.*$", FG_CMT),
        S("'([^']|'')*'", FG_STR),
        S("(?i)\\b(select|from|where|and|or|not|insert|into|values|update|set|delete|create|table|drop|alter|join|left|right|inner|outer|on|group|by|order|limit|offset|as|distinct|union|all|null|primary|key|foreign|references|index|view|begin|commit|rollback|case|when|then|else|end|having|like|in|is|between|exists)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_make", "(^|/)[Mm]akefile$|(^|/)GNUmakefile$|\\.mk$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("^[^:#=\\s][^:#=]*:", FG_KW),
        S("\\$\\([^)]+\\)", FG_PRE),
        S("\\$\\{[^}]+\\}", FG_PRE),
        S("^\\t.*$", FG_STR),
        SYN_END
    });
    add(editor, "syn_diff", "\\.(diff|patch)$", (srule_def_t[]){
        S("^(\\+\\+\\+|---).*$", FG_CMT),
        S("^@@.*$", FG_PRE),
        S("^~.*$", TB_YELLOW | TB_BOLD),
        S("^\\+.*$", FG_KW),
        S("^-.*$", FG_PUN),
        S("^diff .*$", TB_WHITE | TB_BOLD),
        S("^(work tree|staged|untracked|modified|added|deleted|renamed|conflict).*$", TB_WHITE | TB_BOLD),
        S("^[0-9]+ added,.*$", FG_PRE),
        SYN_END
    });
    add(editor, "syn_ini", "\\.(ini|conf|cfg|properties)$", (srule_def_t[]){
        S("[;#].*$", FG_CMT),
        S("^\\s*\\[[^\\]]+\\]", FG_PRE),
        S("^\\s*[A-Za-z0-9_.-]+\\s*=", FG_KW),
        S("\"[^\"]*\"", FG_STR),
        SYN_END
    });
    add(editor, "syn_docker", "(^|/)Dockerfile$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("^\\s*[A-Z][A-Z]+", FG_KW),
        S("\\$\\{[^}]+\\}", FG_PRE),
        S("\"[^\"]*\"", FG_STR),
        SYN_END
    });
    add(editor, "syn_cmake", "(^|/)CMakeLists\\.txt$|\\.cmake$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("\"[^\"]*\"", FG_STR),
        S("\\b(if|else|elseif|endif|foreach|endforeach|while|endwhile|function|endfunction|macro|endmacro|set|project|cmake_minimum_required|add_executable|add_library|target_link_libraries|include|find_package|option|message)\\b", FG_KW),
        S("\\$\\{[^}]+\\}", FG_PRE),
        SYN_END
    });
    add(editor, "syn_hs", "\\.hs$", (srule_def_t[]){
        MM("\\{-", "-\\}", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("--.*$", FG_CMT),
        S("\\b(case|class|data|default|deriving|do|else|if|import|in|infix|infixl|infixr|instance|let|module|newtype|of|then|type|where|family|forall)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_ex", "\\.(ex|exs)$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("#.*$", FG_CMT),
        S("\\b(def|defp|defmodule|defstruct|defprotocol|defimpl|do|end|if|else|unless|cond|case|when|fn|for|with|raise|import|alias|require|use|true|false|nil)\\b", FG_KW),
        S(":[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_lisp", "\\.(clj|cljs|cljc|edn|lisp|el|scm)$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S(";.*$", FG_CMT),
        S("\\b(defn|defn-|def|defmacro|let|if|when|do|fn|loop|recur|ns|require|use|quote|var|cond|case|and|or|not|nil|true|false|lambda|defun|setq|progn)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_r", "\\.[Rr]$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("#.*$", FG_CMT),
        S("\\b(if|else|repeat|while|function|for|in|next|break|TRUE|FALSE|NULL|Inf|NaN|NA|library|require|return)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_nim", "\\.nim$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("#.*$", FG_CMT),
        S("\\b(proc|func|method|iterator|macro|template|converter|if|elif|else|case|of|when|for|while|break|continue|return|yield|discard|block|let|var|const|type|object|enum|tuple|import|export|include|from|as|nil|true|false|and|or|not|xor|div|mod|shl|shr|is|isnot|in|notin)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_proto", "\\.proto$", (srule_def_t[]){
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(syntax|package|import|option|message|enum|service|rpc|returns|repeated|optional|required|map|reserved|oneof|extend|extensions|to|true|false)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_dart", "\\.dart$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)*'", FG_STR),
        S("//.*$", FG_CMT),
        S("\\b(abstract|as|assert|async|await|break|case|catch|class|const|continue|covariant|default|deferred|do|dynamic|else|enum|export|extends|extension|external|factory|false|final|finally|for|Function|get|hide|if|implements|import|in|interface|is|late|library|mixin|new|null|on|operator|part|required|rethrow|return|set|show|static|super|switch|sync|this|throw|true|try|typedef|var|void|while|with|yield)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_powershell", "\\.(ps1|psm1|psd1)$", (srule_def_t[]){
        MM("<#", "#>", FG_CMT),
        S("#.*$", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'[^']*'", FG_STR),
        S("\\$[A-Za-z_][A-Za-z0-9_:]*", FG_PRE),
        S("(?i)\\b(begin|break|catch|class|continue|data|define|do|dynamicparam|else|elseif|end|enum|exit|filter|finally|for|foreach|from|function|if|in|param|process|return|switch|throw|trap|try|until|using|var|while|workflow|true|false|null)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_hcl", "\\.(tf|tfvars|hcl)$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("#.*$", FG_CMT),
        S("//.*$", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("\\$\\{[^}]+\\}", FG_PRE),
        S("^\\s*\\K[A-Za-z_][A-Za-z0-9_-]*(?=\\s+(\"[^\"]+\"\\s*)?\\{)", FG_KW),
        S("^\\s*[A-Za-z_][A-Za-z0-9_-]*\\s*=", FG_KW),
        S("\\b(true|false|null|for|in|if|else)\\b", FG_PRE),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_graphql", "\\.(graphql|gql)$", (srule_def_t[]){
        MM("\"\"\"", "\"\"\"", FG_STR),
        S("#.*$", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("\\$[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b(query|mutation|subscription|fragment|on|schema|scalar|type|interface|union|enum|input|directive|extend|implements|true|false|null)\\b", FG_KW),
        S("@[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_nix", "\\.nix$", (srule_def_t[]){
        MM("/\\*", "\\*/", FG_CMT),
        S("#.*$", FG_CMT),
        MM("''", "''", FG_STR),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("\\$\\{[^}]+\\}", FG_PRE),
        S("\\b(assert|else|if|in|inherit|let|or|rec|then|with|builtins|true|false|null)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_ocaml", "\\.(ml|mli)$", (srule_def_t[]){
        MM("\\(\\*", "\\*\\)", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'([^'\\\\]|\\\\.)'", FG_STR),
        S("\\b(and|as|assert|begin|class|constraint|do|done|downto|else|end|exception|external|false|for|fun|function|functor|if|in|include|inherit|initializer|lazy|let|match|method|module|mutable|new|nonrec|object|of|open|or|private|rec|sig|struct|then|to|true|try|type|val|virtual|when|while|with)\\b", FG_KW),
        S("\\b-?[0-9]+(\\.[0-9]+)?\\b", FG_NUM),
        SYN_END
    });
    add(editor, "syn_env", "(^|/)\\.env(\\.[A-Za-z0-9_-]+)?$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("\"([^\"\\\\]|\\\\.)*\"", FG_STR),
        S("'[^']*'", FG_STR),
        S("^\\s*(export\\s+)?[A-Za-z_][A-Za-z0-9_]*\\s*=", FG_KW),
        S("\\$\\{[^}]+\\}", FG_PRE),
        S("\\$[A-Za-z_][A-Za-z0-9_]*", FG_PRE),
        SYN_END
    });
    add(editor, "syn_gitignore", "(^|/)\\.(gitignore|gitattributes|gitmodules)$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("^(!|\\*)?[^#].*$", FG_STR),
        SYN_END
    });
    add(editor, "syn_git", "(^|/)(git-rebase-todo|COMMIT_EDITMSG)$", (srule_def_t[]){
        S("#.*$", FG_CMT),
        S("^(pick|reword|edit|squash|fixup|exec|break|drop|label|reset|merge|update-ref)\\b", FG_KW),
        S("\\b[0-9a-f]{7,40}\\b", FG_NUM),
        SYN_END
    });
}

#undef S
#undef MM
#undef SYN_END
#undef FG_STR
#undef FG_CMT
#undef FG_KW
#undef FG_NUM
#undef FG_PUN
#undef FG_PRE

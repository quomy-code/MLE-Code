#!/usr/bin/env bash
tmpn=test_syntax_file
extra_opts=(-S syn_test,$tmpn,4,0 -s foo,,258,3 $tmpn)
macro='M-c f o o'
declare -A expected
expected[style_fg]='\bfg=258\b'
expected[style_bg]='\bbg=3\b'
source 'test.sh'

on_exit() { [ -n "$tmpf" ] && rm -f $tmpf; }
trap on_exit EXIT
tmpf=$(mktemp 'mle.test_syntax.XXXXXXXXXX')

cat >$tmpf <<"EOD"
resource "demo" {
  enabled = true
  count = 3
}
# note
EOD
extra_opts=(-y syn_hcl $tmpf)
macro=''
declare -A expected
expected[hcl_keyword]='<ch=114 fg=3\b'
expected[hcl_string]='<ch=100 fg=260\b'
expected[hcl_boolean]='<ch=116 fg=262\b'
expected[hcl_comment]='<ch=110 fg=7\b'
source 'test.sh'

cat >$tmpf <<"EOD"
function Test-Value {
  $answer = $true
  # note
}
EOD
extra_opts=(-y syn_powershell $tmpf)
macro=''
declare -A expected
expected[ps_keyword]='<ch=102 fg=3\b'
expected[ps_variable]='<ch=36 fg=262\b'
expected[ps_comment]='<ch=110 fg=7\b'
source 'test.sh'

cat >$tmpf <<"EOD"
pick 0123456 subject
# note
EOD
extra_opts=(-y syn_git $tmpf)
macro=''
declare -A expected
expected[git_keyword]='<ch=112 fg=3\b'
expected[git_hash]='<ch=48 fg=261\b'
expected[git_comment]='<ch=110 fg=7\b'
source 'test.sh'

tmpn=test_syntax_file
extra_opts=(-S syn_test,$tmpn,4,0 -s foo,,1,2 $tmpn)
macro='M-c { enter f o o ; enter } enter M-\ M-a M-/ M-. M-a'
declare -A expected
expected[outdent_style]='<ch=102 fg=1 bg=2><ch=111 fg=1 bg=2><ch=111 fg=1 bg=2><ch=59 fg=0 bg=0>'
source 'test.sh'

cat >$tmpf <<"EOD"
"aaa"
"aaa\\"
"aaa \" aaa"
"aaa"          bbb "aaa"
"aaa \" aaa"   bbb "aaa \" aaa"
"aaa \" aaa\\" bbb "aaa \" aaa\\"
"aaa"          bbb
"aaa\\"        bbb
"aaa \" aaa"   bbb
"aaa"          bbb "aaa"          bbb
"aaa \" aaa"   bbb "aaa \" aaa"   bbb
"aaa \" aaa\\" bbb "aaa \" aaa\\" bbb
bbb "aaa"          bbb
bbb "aaa\\"        bbb
bbb "aaa \" aaa"   bbb
bbb "aaa"          bbb "aaa"          bbb
bbb "aaa \" aaa"   bbb "aaa \" aaa"   bbb
bbb "aaa \" aaa\\" bbb "aaa \" aaa\\" bbb
EOD
extra_opts=(-yy $tmpf)
macro=''
declare -A expected
declare -A not_expected
# a==97 b==98
expected[style_dq_a]='<ch=97 fg=260\b'
expected[style_dq_b]='<ch=98 fg=0\b'
not_expected[style_dq_not_a]='<ch=97 fg=0\b'
not_expected[style_dq_not_b]='<ch=98 fg=260\b'
source 'test.sh'

cat >$tmpf <<"EOD"
"aa"         /*bb*/
"aa/*aa"     /*bb*/
"aa/*aa*/aa" /*bb*/
"aa"         /*bb*/
"aa/*aa"     /*bb"bb*/
"aa/*aa*/aa" /*bb"bb"bb*/
/*bb*/       "aa"
/*bb*/       "aa/*aa"
/*bb*/       "aa/*aa*/aa"
/*bb*/       "aa"
/*bb"bb*/    "aa/*aa"
/*bb"bb"bb*/ "aa/*aa*/aa"
EOD
extra_opts=(-yy $tmpf)
macro=''
declare -A expected
declare -A not_expected
# a==97 b==98
expected[style_sc_a]='<ch=97 fg=260\b'
expected[style_sc_b]='<ch=98 fg=7\b'
not_expected[style_sc_not_a]='<ch=97 fg=(0|7)\b'
not_expected[style_sc_not_b]='<ch=98 fg=(0|260)\b'
source 'test.sh'

cat >$tmpf <<"EOD"
# ccc
#
aaa "bbb" // ccc "ccc"
aaa "bbb" # ccc "ccc"
EOD
extra_opts=(-yy $tmpf)
macro=''
declare -A expected
declare -A not_expected
# a==97 b==98 c==99 #=35
expected[style_a]='<ch=97 fg=0\b'
expected[style_b]='<ch=98 fg=260\b'
expected[style_c]='<ch=99 fg=7\b'
expected[style_h]='<ch=35 fg=7\b'
not_expected[style_not_a]='<ch=97 fg=(260|7)\b'
not_expected[style_not_b]='<ch=98 fg=(0|7)\b'
not_expected[style_not_c]='<ch=99 fg=(0|260)\b'
not_expected[style_not_h]='<ch=35 fg=(0|260)\b'
source 'test.sh'

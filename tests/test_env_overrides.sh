#!/bin/sh
# Every setting on the settings page (source/settings.c) can be overridden at env as BO3MACFIX_<NAME IN CAPITALS>:
# bo3macfix.c reads it with read_int_setting_or_env / read_string_setting_or_env, or the module that gets the value
# reads that variable itself. A plain read_int_setting("Name") with neither reads settings.ini only.
cd "$(dirname "$0")/../source" || exit 1
failed=0
for name in $(sed -n 's/^    { "\([A-Za-z0-9]*\)",.*/\1/p' settings.c); do
    upper=$(echo "$name" | tr '[:lower:]' '[:upper:]')
    grep -q "_or_env(\"$name\"" ./*.c ./*.m && continue
    grep -q "\"BO3MACFIX_$upper\"" ./*.c ./*.m ../launcher/*.m && continue
    echo "FAIL: $name has no BO3MACFIX_$upper override"
    failed=1
done
[ $failed = 0 ] && echo "env overrides: every setting has one"
exit $failed

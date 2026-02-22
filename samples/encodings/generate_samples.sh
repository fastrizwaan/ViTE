#!/usr/bin/env sh
set -eu

out_dir="${1:-samples/encodings/files}"
mkdir -p "$out_dir"

# Keep this list in sync with src/piece-table.c encoding_infos[]
encodings='utf-8 utf-16le utf-16be utf-32le utf-32be iso-8859-1 iso-8859-2 iso-8859-3 iso-8859-4 iso-8859-5 iso-8859-6 iso-8859-7 iso-8859-8 iso-8859-9 iso-8859-10 iso-8859-11 iso-8859-13 iso-8859-14 iso-8859-15 iso-8859-16 windows-1250 windows-1251 windows-1252 windows-1253 windows-1254 windows-1255 windows-1256 windows-1257 windows-1258 koi8-r koi8-u cp850 cp852 cp855 cp857 cp862 cp864 cp866 shift_jis euc-jp iso-2022-jp gb18030 gbk big5 big5-hkscs euc-kr cp949 iso-2022-kr tis-620'

sample_for_encoding() {
    enc="$1"
    case "$enc" in
        windows-1251|iso-8859-5|koi8-r|koi8-u|cp866|cp855)
            printf '%s\n' 'ViTE sample: Привет мир 123';;
        windows-1253|iso-8859-7)
            printf '%s\n' 'ViTE sample: Γειά σου κόσμε 123';;
        windows-1256|iso-8859-6|cp864)
            printf '%s\n' 'ViTE sample: مرحبا بالعالم 123';;
        windows-1255|iso-8859-8|cp862)
            printf '%s\n' 'ViTE sample: שלום עולם 123';;
        windows-1254|iso-8859-9|cp857)
            printf '%s\n' 'ViTE sample: Merhaba dünya Iğdır 123';;
        tis-620|iso-8859-11)
            printf '%s\n' 'ViTE sample: สวัสดีชาวโลก 123';;
        shift_jis|euc-jp|iso-2022-jp)
            printf '%s\n' 'ViTE sample: こんにちは世界 123';;
        gb18030|gbk|big5|big5-hkscs)
            printf '%s\n' 'ViTE sample: 你好世界 繁體 123';;
        euc-kr|cp949|iso-2022-kr)
            printf '%s\n' 'ViTE sample: 안녕하세요 세계 123';;
        windows-1250|iso-8859-2|cp852)
            printf '%s\n' 'ViTE sample: Zażółć gęślą jaźń 123';;
        windows-1257|iso-8859-13)
            printf '%s\n' 'ViTE sample: Sveiki pasaule ĀČĒĢ 123';;
        windows-1258)
            printf '%s\n' 'ViTE sample: Xin chào Việt Nam 123';;
        *)
            printf '%s\n' 'ViTE sample: Café naïve résumé € 123';;
    esac
}

iconv_name() {
    enc="$1"
    case "$enc" in
        utf-16le) printf '%s' 'UTF-16LE';;
        utf-16be) printf '%s' 'UTF-16BE';;
        utf-32le) printf '%s' 'UTF-32LE';;
        utf-32be) printf '%s' 'UTF-32BE';;
        windows-*) printf '%s' "${enc#windows-}" | awk '{print "WINDOWS-"$0}';;
        shift_jis) printf '%s' 'SHIFT_JIS';;
        euc-jp) printf '%s' 'EUC-JP';;
        iso-2022-jp) printf '%s' 'ISO-2022-JP';;
        gb18030) printf '%s' 'GB18030';;
        gbk) printf '%s' 'GBK';;
        big5) printf '%s' 'BIG5';;
        big5-hkscs) printf '%s' 'BIG5-HKSCS';;
        euc-kr) printf '%s' 'EUC-KR';;
        cp949) printf '%s' 'CP949';;
        iso-2022-kr) printf '%s' 'ISO-2022-KR';;
        tis-620) printf '%s' 'TIS-620';;
        koi8-r) printf '%s' 'KOI8-R';;
        koi8-u) printf '%s' 'KOI8-U';;
        cp*) printf '%s' "$(printf '%s' "$enc" | tr '[:lower:]' '[:upper:]')";;
        iso-8859-*) printf '%s' "$(printf '%s' "$enc" | tr '[:lower:]' '[:upper:]')";;
        utf-8) printf '%s' 'UTF-8';;
        *) printf '%s' "$enc";;
    esac
}

manifest="$out_dir/MANIFEST.tsv"
report="$out_dir/VALIDATION.txt"
: > "$manifest"
: > "$report"
printf 'encoding_id\ticonv_name\tbytes\tconvert_mode\tdecode_check\n' >> "$manifest"

for enc in $encodings; do
    src_tmp="$out_dir/.src-${enc}.txt"
    sample_for_encoding "$enc" > "$src_tmp"

    tgt="$(iconv_name "$enc")"
    out_file="$out_dir/sample.${enc}.txt"

    mode='strict'
    if iconv -f UTF-8 -t "$tgt" "$src_tmp" > "$out_file" 2>/dev/null; then
        mode='strict'
    elif iconv -f UTF-8 -t "${tgt}//TRANSLIT" "$src_tmp" > "$out_file" 2>/dev/null; then
        mode='translit'
    elif iconv -f UTF-8 -t "${tgt}//IGNORE" "$src_tmp" > "$out_file" 2>/dev/null; then
        mode='ignore'
    else
        printf 'FAILED conversion: %s (%s)\n' "$enc" "$tgt" >> "$report"
        rm -f "$src_tmp"
        continue
    fi

    # Add BOM for explicit-endian UTF files for detection convenience
    case "$enc" in
        utf-16le)
            bom_file="$out_dir/.bom-${enc}.bin"
            printf '\377\376' > "$bom_file"
            cat "$bom_file" "$out_file" > "$out_file.tmp" && mv "$out_file.tmp" "$out_file"
            rm -f "$bom_file"
            ;;
        utf-16be)
            bom_file="$out_dir/.bom-${enc}.bin"
            printf '\376\377' > "$bom_file"
            cat "$bom_file" "$out_file" > "$out_file.tmp" && mv "$out_file.tmp" "$out_file"
            rm -f "$bom_file"
            ;;
        utf-32le)
            bom_file="$out_dir/.bom-${enc}.bin"
            printf '\377\376\000\000' > "$bom_file"
            cat "$bom_file" "$out_file" > "$out_file.tmp" && mv "$out_file.tmp" "$out_file"
            rm -f "$bom_file"
            ;;
        utf-32be)
            bom_file="$out_dir/.bom-${enc}.bin"
            printf '\000\000\376\377' > "$bom_file"
            cat "$bom_file" "$out_file" > "$out_file.tmp" && mv "$out_file.tmp" "$out_file"
            rm -f "$bom_file"
            ;;
    esac

    bytes=$(wc -c < "$out_file" | tr -d ' ')

    if iconv -f "$tgt" -t UTF-8 "$out_file" >/dev/null 2>&1; then
        decode='ok'
    else
        decode='failed'
        printf 'FAILED decode check: %s (%s)\n' "$enc" "$tgt" >> "$report"
    fi

    printf '%s\t%s\t%s\t%s\t%s\n' "$enc" "$tgt" "$bytes" "$mode" "$decode" >> "$manifest"
    rm -f "$src_tmp"
done

printf 'Generated samples in %s\n' "$out_dir" >> "$report"

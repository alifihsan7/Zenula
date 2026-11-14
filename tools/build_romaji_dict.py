"""Builds the romaji add-on for Zenula from JMdict.

Usage: python tools/build_romaji_dict.py JMdict_e.gz romaji-ja.tsv

JMdict_e.gz comes from the Electronic Dictionary Research and Development Group:
http://ftp.edrdg.org/pub/Nihongo/JMdict_e.gz (CC BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html).

The output is UTF-8 text, one "word<TAB>reading" per line, sorted by the word's UTF-8 bytes so Zenula can
binary search it. Words are the kanji spellings from JMdict; readings are in hiragana. Each verb or adjective
also adds its stem (歩く/あるく gives 歩/ある), so inflected lyrics like 歩いて still find a reading.
"""
import gzip
import sys
import xml.etree.ElementTree as ET

MAX_KEY = 12


def has_kanji(text):
    return any('一' <= ch <= '鿿' or ch == '々' for ch in text)


def is_hiragana(ch):
    return 'ぁ' <= ch <= 'ゟ'


def to_hiragana(text):
    return ''.join(chr(ord(ch) - 0x60) if 'ァ' <= ch <= 'ヶ' else ch for ch in text)


# Godan verbs change their last kana when they inflect: 探す -> 探し(て), 歩く -> 歩い(て), 読む -> 読ん(で).
GODAN = {
    'う': 'わいえおっ', 'く': 'かきけこい', 'ぐ': 'がぎげごい', 'す': 'さしせそ', 'つ': 'たちてとっ',
    'ぬ': 'なにねのん', 'ぶ': 'ばびべぼん', 'む': 'まみめもん', 'る': 'らりれろっ',
}


def main(source, target):
    words = {}   # spelling -> (reading, common)
    forms = {}   # inflected stems of godan verbs
    stems = {}
    kana_words = {}   # common words usually written in kana (ここ, だけ, ありがとう), to split runs of kana

    def offer(table, key, reading, common, needs_kanji=True):
        if not key or len(key) > MAX_KEY or (needs_kanji and not has_kanji(key)):
            return
        old = table.get(key)
        if old is None or (common and not old[1]):
            table[key] = (reading, common)

    with gzip.open(source, 'rb') as stream:
        for _, entry in ET.iterparse(stream, events=('end',)):
            if entry.tag != 'entry':
                continue
            readings = []
            for r in entry.findall('r_ele'):
                if r.find('re_nokanji') is not None:
                    continue
                readings.append((to_hiragana(r.findtext('reb', '')), [x.text for x in r.findall('re_restr')],
                                 r.find('re_pri') is not None))
            pos_text = ' '.join(pos.text or '' for pos in entry.iter('pos'))
            godan = 'Godan verb' in pos_text
            ichidan = 'Ichidan verb' in pos_text
            kuru = 'Kuru verb' in pos_text
            # Whole phrases (今は, 迎えに来る) would swallow their particles; words alone split better.
            if 'expressions' in pos_text.lower():
                entry.clear()
                continue
            for r in entry.findall('r_ele'):
                spelling = r.findtext('reb', '')
                if r.find('re_pri') is not None and 2 <= len(spelling) <= 8:
                    offer(kana_words, spelling, to_hiragana(spelling), True, needs_kanji=False)
            for k in entry.findall('k_ele'):
                spelling = k.findtext('keb', '')
                common = k.find('ke_pri') is not None
                reading = next((rb for rb, restr, _ in readings if not restr or spelling in restr), None)
                if not reading:
                    continue
                offer(words, spelling, reading, common)
                last = spelling[-1]
                if godan and last in GODAN and reading.endswith(last):
                    for variant in GODAN[last]:
                        offer(forms, spelling[:-1] + variant, reading[:-1] + variant, common)
                if ichidan and last == 'る' and reading.endswith('る') and len(spelling) > 1:
                    offer(forms, spelling[:-1], reading[:-1], common)   # 伝える -> 伝え
                if kuru:
                    # 来る is irregular: 来た is きた, 来ない is こない, 来れば is くれば.
                    if spelling.endswith('来る') and reading.endswith('くる'):
                        head, sound = spelling[:-2], reading[:-2]
                        for kana, read in (('た', 'きた'), ('て', 'きて'), ('ま', 'きま'), ('な', 'こな'), ('い', 'こい'),
                                           ('よ', 'こよ'), ('ら', 'こら'), ('れ', 'くれ'), ('', 'き')):
                            offer(forms, head + '来' + kana, sound + read, True)
                    continue   # no generic stem for 来る
                # Stem: drop the trailing kana both forms share (歩く/あるく -> 歩/ある).
                tail = len(spelling)
                while tail > 0 and is_hiragana(spelling[tail - 1]):
                    tail -= 1
                suffix = spelling[tail:]
                if suffix and tail > 0 and reading.endswith(suffix) and len(reading) > len(suffix):
                    offer(stems, spelling[:tail], reading[:-len(suffix)], common)
            entry.clear()

    for table in (forms, stems, kana_words):
        for key, value in table.items():
            if key not in words:
                words[key] = value

    lines = sorted((key.encode('utf-8'), reading.encode('utf-8')) for key, (reading, _) in words.items())
    with open(target, 'wb') as out:
        out.write('# Zenula romaji dictionary, built from JMdict (EDRDG, CC BY-SA 4.0)\n'.encode('utf-8'))
        for key, reading in lines:
            out.write(key + b'\t' + reading + b'\n')
    print(f'{len(lines)} words written to {target}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])

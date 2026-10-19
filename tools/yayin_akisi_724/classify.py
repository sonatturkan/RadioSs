"""Rule-based music categories for the RadioS library (no genre tags exist)."""
import re, json, sys, collections, unicodedata

TR_CHARS = re.compile(r"[çğışÇĞİŞ]")
def nfc(s): return unicodedata.normalize("NFC", s or "")
def norm(s): return nfc(s).lower().replace("İ", "i")

TURKISH = """tarkan|sezen aksu|ajda pekkan|kenan dogulu|kenan doğulu|hande yener|mustafa sandal|hadise|demet akalin|demet akalın|sertab erener|gülşen|gulsen|edis|atiye|manga|emrah karaduman|murat dalkilic|simge|sila|teoman|athena|bengü|bengu|göksel|goksel|nil karaibrahimgil|serdar ortac|serdar ortaç|ozan dogulu|ozan doğulu|ozan çolakoğlu|ozan colakoglu|zeynep bastik|zeynep bastık|reynmen|sefo|ezhel|norm ender|ragga oktay|mabel matiz|can bonomo|kargo|duman|yüksek sadakat|hakan peker|emre altug|emre altuğ|nazan öncel|yildiz tilbe|yıldız tilbe|aşkin nur yengi|aşkın nur yengi|seden gürel|rengin|ece seçkin|ekin beril|gökçe|tuğba yurt|tuğçe|izel|çelik|yaşar|metin arolat|mehmet güreli|tamer kaan|neval|nalan|güneş|irmak arici|irmak arıcı|hande mehan|doğuş|dogus cihan|harun kolçak|miret|ayça|berksan|kanun yildirim|kanun yıldırım|emir taha|ahmet enes|şirin soysal|mert demir|uzi|murda|burry soprano|ege balkiz|taylan kaya|ferdi ozbegen|bedük|ayse hatun|emel müftüoğlu|gökhan türkmen|ardiç duygu|ardıç duygu|cem pilevneli|volkan uca|evrencan|lemurian|zeynep|su na|tuna kiremitçi|kaçın kurası|alem-i adastra|anladım ben seni|bana ellerini ver|benimle oynama|boşvermişim|yeter ki onursuz|iyi ki doğdun|i̇yi ki doğdun|çalkala|mansur ark|dj dali|tuğçe şenoğul|reyhan karaca|sura iskəndərli|deniz kabu|ilkay sencan|mezdeke|hassan abou|mary jane|fatih ürek|main records|bedük|öpücük"""
DJ_TR_DANCE = ["ilkay sencan", "burak yeter", "deniz kabu", "cem pilevneli", "volkan uca", "lemurian", "mahmut orhan", "ummet ozcan"]

CLASSICS = """abba|bee gees|boney m|bonnie tyler|blondie|bon jovi|cher|corona|crystal waters|culture club|cyndi lauper|dalida|depeche mode|desireless|dr. alban|edith piaf|elton john|elvis presley|enigma|enya|europe|eurythmics|frank sinatra|freddie mercury|george michael|gipsy kings|gloria gaynor|herb alpert|james brown|julio iglesias|kool & the gang|laura branigan|lionel richie|lou bega|madonna|marty robbins|michael jackson|modern talking|nancy sinatra|nirvana|patrick hernandez|pink floyd|queen|ray charles|reel 2 real|rick astley|robert miles|robin s|roxette|savage garden|snap!|soul ii soul|spice girls|suzanne vega|the beatles|the doors|the police|tina turner|toto|vaya con dios|wham!|whitney houston|yazoo|a-ha|alphaville|ben e. king|aretha franklin|bobby mcferrin|chaka demus|cerrone|idris muhammad|goran bregovi|paolo conte|hermes house band|bloodhound gang|mariah carey|t.a.t.u.|darude|moloko|modjo|fatboy slim|groove armada|royksopp|shaggy|crazy frog|tom jones|rusted root|opus|live is life|macarena|the belle stars|caro emerald|gotan project|daft punk|basement jaxx|atb|the white stripes|soul ii soul|kelis|nelly furtado|savage garden|celine|ricky martin|haddaway|2 unlimited|eiffel 65|vengaboys|scatman|dr alban|sandstorm|groovy 69|village people|mr. president|la bouche|real mccoy|ace of base|culture beat|snap|the weather girls|weather girls|crystal waters|milk & sugar with barbara tucker"""

URBAN = """21 savage|50 cent|4tgang|akon|azealia banks|becky g|beyoncé|beyonce|black eyed peas|britney spears|busta rhymes|ckay|chief keef|craig david|daddy yankee|desiigner|don omar|doja cat|dr. dre|drake|eminem|era istrefi|far east movement|farruko|french montana|jason derulo|jay-z|j balvin|j. balvin|kanye west|kid cudi|migos|missy elliott|natti natasha|partynextdoor|pitbull|post malone|rae sremmurd|rema|roddy ricch|saweetie|sean paul|timbaland|tyga|wiz khalifa|young thug|young t & bugsey|goya menor|saint jhn|kmt feat giggs|y2k|bbno$|lil nas x|tokyo drift|danza kuduro|pepas|gusttavo lima|nayer|nicki minaj|will.i.am|lmfao|pharrell|swae lee|french & swae|katy perry ft. migos|shakira|karol g|ozuna|maluma|bad bunny|chris brown|travis scott|cardi b|snoop|usher|ne-yo|the weeknd|ludacris|sickick|talk dirty|worth it|turning me up|milkshake"""

POP = """adele|ava max|alice merton|alan walker|benee|bruno mars|camila cabello|capital cities|charlie puth|clean bandit|dennis lloyd|dua lipa|ed sheeran|elley duhé|empire of the sun|enrique iglesias|fifth harmony|foster the people|gayle|icona pop|katy perry|kat deluna|lady gaga|lana del rey|lilly wood|little mix|måneskin|maneskin|mark ronson|marshmello|miley cyrus|maroon 5|omi|portugal. the man|rihanna|sam smith|selena gomez|shawn mendes|stromae|tones and i|tones & i|tom odell|zaz|alexandra stan|carla's dreams|carla’s dreams|otilia|maruv|edward maya|sak noel|lena|arnon|mr. saik|silva gunbardhi|mi gente|taio cruz|imagine dragons|sia|ellie goulding|jessie j|pink|avril|coldplay|onerepublic|one direction|harry styles|billie eilish|olivia rodrigo|ariana grande|justin bieber|bebe rexha|tove lo|marian hill|missio|fousheé|aya|dalida|kate linn|gala|twin|polina|brianna|mariline|ana tijoux|cléa vincent|men i trust|konoba|hadiya george|simone white|yoav|lou bega|loreen|inna|alexandra|crazy|caro emerald|el ritmo|lego|walking on air|this is love|señorita|could you be loved|tom's diner|camila"""

HOUSE_DEEP = """claptone|duke dumont|adana twins|tv noise|valeron|josh butler|john summit|deeperise|mahmut orhan|gorgon city|dom dolla|fisher|chris lake|solomun|monolink|rüfüs du sol|rufus du sol|nora en pure|finnebassen|kisch|kevin mckay|milk & sugar|croatia squad|chico rose|dr. kucho|gregor salto|funkerman|infinity ink|shadow child|ben pearce|weiss|mark knight|junior sanchez|pete tong|shouse|sg lewis|fred again|nicolas jaar|gesaffelstein|kant|moreno pezzolato|namito|oliver koletzki|raving george|tone depth|hosh|sebjak|klaas|damian lazarus|ferreck dawn|kasper koman|michael calfan|watermät|watermat|sofi tukker|zhu|disko|yvvan back|deepanima|drenchill|nora & chris|ytram|bleu clair|kda|mk|purple disco machine|riton|kadosh|argy|anton ishutin|axel thesleff|ben delay|ben c & kalsx|bontan|cameo club|citadelle|degiheugi|départ|drechsler|eauxmar|endor|estiva|feb & rich james|filv|frank leone|frey & waxy|gaullin|going deeper|hafex|hwls|inndrive|jarami|jend|jenia tarsol|jewel kid|jimmy & fred|kadebostany|kovacs|late night alumni|leftwing|liu|loothief|lothief|losless|majestic|mia dora|midi culture|moksi|münch|munch|nightmorss|nils ohrmann|nu & jo ke|nytron|olly walker|osrin|ozbi|p.a.c.o.|pig&dan|premiere|promise land|redlight|regard|revelries|rune rk|sammy porter|semedo|serge devant|silque|silverleaf|simone vitullo|sleepy chows|space dimension controller|steve void|sunflwr|syzz|t. harding|taiki nulight|themba|tom bourra|tony romera|tru concept|tuccillo|victor siriani|vovawave|wade|daniel steinberg|daddy's groove|dayne s|deep down|oliver $|jimi jules|mantu|onen|solmaz lou|magnificence|sir julian|vetlove|yoav|alberto jossue|brams|rummy sharma|helion|olympis|piero pirupa|music p|malik mustache|n.e.o.n|surf mesa|gaullin|stadiumx|scott rill|reboot|ralph felix|nice & wild|slh|smack|steff da campo|thurisaz|triticum|trinix|untrusted|vintage culture|camelphat|meduza|nightcrawlers|robin schulz|lost frequencies|kungs|ofenbach|synapson|bellagio|dzeko|fever|focus fire|cazztek|wolfson|deniz kabu|faul|wad ad|pnau|kean dysso|ricii|ralvero|edx|bob sinclar|joy corporation|cat dealers|bruno martini|flakkë|silvio luz|soul|hugel|imanbek"""

EDM = """martin garrix|david guetta|avicii|calvin harris|afrojack|alesso|armin van buuren|axwell|bassjackers|blasterjaxx|bingo players|dvbbs|deorro|dimitri vegas|don diablo|dyro|hardwell|jay hardway|julian jordan|kshmr|loopers|madison mars|matisse & sadko|quintino|r3hab|showtek|steve aoki|swedish house mafia|tiësto|tiesto|timmy trumpet|tujamo|ummet ozcan|w&w|burak yeter|alok|dynoro|galantis|major lazer|dj snake|skrillex|jack ü|diplo|zedd|yellow claw|will sparks|carnage|salvatore ganacci|sandro silva|rnbstylerz|brooks|garmiani|breaking beattz|vicetone|fedde le grand|sidney samson|oliver heldens|acraze|area21|best of edm|besomorph|blinders|bnry|breach|badflite|beatamines|blr|bang la decks|bart b more|cmc$|grx|crunkz|dj fresh|dmnds|dubdogz|dubvision|eric prydz|erd1|flexking|freejak|gable|high rankin|hl:dr|ibranovski|julian jordan|justin mylo|k-billy|keanu silva|kvsh|lvbel c5|loop & echo|masn|matt nash|melon & rezidential|mwm|no method|pontifexx|prezioso|reebok|rebuke|route 94|saison|sean finn|slider & magnit|stickmen project|syzz|taska black|thatskramer|tjr|todd helder|triplo max|twerkillaz|vinai|zerb|chico rose|71 digits|dj katch|djs from mars|dj goja|goja|dj dark|anton|walking|ilkay sencan|deniz koyu|contiez|charles j|disko|endor|fella|trfn|bleech|mr. saik|crispie|forever|aaron smith|afraid to feel|alex gaudino|bag raiders|badman|bellagio|darude|dj s-wolf|jax jones|james hype|jack perry|john junior|nils van zandt|robka|nicola fasano|olly walker|sak noel|showtek|topic|a7s|goodboys|lizot|majestic|vini vici|wolfpack|moguai|headhunterz|mesto|sevenn|solardo|kid ink|lotten|plastik funk|vinne|murotani|nightcrawlers|hugel|imanbek|byor|gia koka|willy william|☑|✅|rich james|zhu|the stickmen|teriyaki boyz|mike gudmann|ralvero"""

KEYWORD_DANCE = re.compile(r"(remix|extended|club mix|original mix|bootleg|vip\b|mashup|\bedit\b|dub mix|rework|remake|tech house|deep house|\bmix\)|mix\]|flip)", re.I)
MASHUP = re.compile(r"(mashup|\bx\b.*\bx\b| x .*\(.*(mashup|edit|remake)|live mashup|played live|\+ more)", re.I)
SLOW_HINT = re.compile(r"(akustik|acoustic|ballad|piano|slow|unplugged)", re.I)

def lists(text): return [t.strip() for t in text.split("|") if t.strip()]
L_TR, L_CL, L_UR, L_POP, L_HD, L_EDM = map(lists, (TURKISH, CLASSICS, URBAN, POP, HOUSE_DEEP, EDM))

_RX = {}
def has(names, hay):
    """Whole-word match so "abba" never hits "jabbar" or "cher" "cherish"."""
    key = id(names)
    rx = _RX.get(key)
    if rx is None:
        alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
        rx = _RX[key] = re.compile(r"(?:^|[^0-9a-zçğıöşü])(?:" + alt + r")(?:$|[^0-9a-zçğıöşü])")
    return bool(rx.search(hay))

CLASSIC_TITLES = ["serenade", "died in your arms", "i love you baby", "fly me to the moon", "gonna make you sweat",
                  "hit the road jack", "pliers bam bam", "losing my religion", "show me love", "titanic"]

def classify(rec):
    m, v = rec["media"], rec["version"]
    title, artist = (m.get("title") or ""), (m.get("artist") or "")
    t, a = norm(title), norm(artist)
    both = a + " || " + t
    dur = v.get("duration_ms", 0) / 1000.0
    bpm = ((rec.get("analysis") or {}).get("rhythm") or {}).get("bpm") or 0
    if t.startswith("going home - sfx") or t.startswith("going home - vo") or t.startswith("untitled_1#03"):
        return "EXCLUDE"
    if dur >= 12 * 60 and (("mix" in t) or ("ep" in t.split()) or "classics by" in t or "tracks in" in t or "visual ep" in t):
        return "DJSET"
    turkish = bool(TR_CHARS.search(nfc(title + artist))) or has(L_TR, a) or (not a and has(L_TR, t))
    dj_tr = any(d in a for d in DJ_TR_DANCE)
    if turkish and not dj_tr:
        return "TR"
    if MASHUP.search(title) and ("mashup" in t or " x " in t or "+ more" in t or "played live" in t):
        return "MASHUP"
    if a == "black" or has(L_CL, a) or (not a and has(CLASSIC_TITLES, t)): return "CLASSIC"
    if has(L_UR, a) or (not a and has(L_UR, t)): return "URBAN"
    if has(L_HD, a) and not has(L_EDM, a): return "HOUSE"
    if has(L_EDM, a) or dj_tr: return "EDM"
    if has(L_POP, a) or (not a and has(L_POP, t)): return "POP"
    if has(L_HD, t): return "HOUSE"
    # Fallbacks on tempo and title keywords.
    if bpm and bpm < 98 and not KEYWORD_DANCE.search(title): return "CHILL"
    if KEYWORD_DANCE.search(title):
        return "HOUSE" if bpm and 118 <= bpm < 126 else "EDM"
    if bpm and bpm >= 118: return "HOUSE" if bpm < 126 else "EDM"
    return "POP"

DEEP = """deeperise|mahmut orhan|finnebassen|monolink|nora en pure|odesza|men i trust|nicolas jaar|rüfüs du sol|kadebostany|konoba|simone white|yoav|lemurian|moreno pezzolato|namito|oliver koletzki|raving george|tone depth|hosh|damian lazarus|deepanima|going deeper|late night alumni|sunflwr|ytram|elderbrook|degiheugi|citadelle|silque|jarami|enigma|solomun|argy|anton ishutin|axel thesleff|endor|estiva|filv|hafex|jenia tarsol|liu|mia dora|midi culture|moksi|nightmorss|nils ohrmann|nu & jo ke|nytron|osrin|ozbi|promise land|semedo|serge devant|silverleaf|simone vitullo|sleepy chows|space dimension controller|steve void|syzz|themba|tony romera|tuccillo|victor siriani|vovawave|wade|alberto jossue|malik mustache|kadosh|münch|gotan project|bontan|cléa vincent|départ|eauxmar|frank leone|hwls|inndrive|jewel kid|kovacs|losless|olympis|helion|sir julian|magnificence|vetlove|gesaffelstein|kant|infinity ink|watermät|disko|weiss|ben c & kalsx|ben pearce|shadow child|regard|drenchill|nora & chris|kisch|kevin mckay"""
L_DEEP = lists(DEEP)
DEEP_TITLE = re.compile(r"(deep house|deep mix|melodic|organic|chill|lounge|downtempo|afro house|\bdeep\b)", re.I)

def chill_override(rec, cat):
    """Slow songs of vocal categories form the late-night/Sunday chill pool."""
    m = rec["media"]; t = m.get("title") or ""
    bpm = ((rec.get("analysis") or {}).get("rhythm") or {}).get("bpm") or 0
    a = norm(m.get("artist"))
    if cat == "HOUSE" and (has(L_DEEP, a) or DEEP_TITLE.search(t)): return "DEEP"
    if cat == "CHILL": return "POP"
    return cat

if __name__ == "__main__":
    recs = json.load(open(sys.argv[1] + "/media.json"))
    cats = collections.defaultdict(list)
    for r in recs:
        c = chill_override(r, classify(r)); cats[c].append(r)
    for c, rows in sorted(cats.items(), key=lambda x: -len(x[1])):
        hours = sum(x["version"]["duration_ms"] for x in rows) / 3.6e6
        bpms = [((x.get("analysis") or {}).get("rhythm") or {}).get("bpm") or 0 for x in rows]
        known = [b for b in bpms if b]
        print("%-8s %4d parça %5.1f saat  medyan BPM %s" % (c, len(rows), hours, round(sorted(known)[len(known)//2]) if known else "-"))
    json.dump({c: [r["media"]["_id"] for r in rows] for c, rows in cats.items()}, open(sys.argv[1] + "/categories.json", "w"))
    if len(sys.argv) > 2:
        for c in sys.argv[2].split(","):
            print("\n== %s ==" % c)
            print(" | ".join(sorted("%s - %s" % ((r["media"].get("artist") or "?")[:28], (r["media"].get("title") or "")[:40]) for r in cats[c])))

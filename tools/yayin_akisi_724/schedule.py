"""Weekly 7/24 format: categories, rotations (dayparts) and the weekly clock."""
CATS = {  # key: playlist name
    "TR": "7/24 · Türkçe Pop", "POP": "7/24 · Pop Hitleri", "URBAN": "7/24 · Urban & Latin",
    "EDM": "7/24 · Dans & EDM", "HOUSE": "7/24 · House & Club", "DEEP": "7/24 · Deep & Melodic",
    "CLASSIC": "7/24 · Klasikler (Gold)", "MASHUP": "7/24 · Mashup & Parti", "DJSET": "7/24 · DJ Set & Megamix",
}
# Transition presets: (mode, weight, overlap_ms, effect). 0 EOF, 1 crossfade, 2 cue/AutoDJ OUT, 3 beatmatch.
T_POP = [(1, 55, 3500, "none"), (2, 45, 2500, "none")]
T_MIX = [(1, 45, 4000, "none"), (2, 25, 3000, "none"), (3, 30, 8000, "none")]
T_DANCE = [(1, 35, 5000, "none"), (3, 65, 12000, "none")]
T_CLUB = [(1, 25, 6000, "none"), (3, 75, 16000, "none")]
T_NIGHT = [(1, 40, 6000, "lowpass"), (2, 20, 3000, "none"), (3, 40, 12000, "none")]
T_SOFT = [(1, 70, 6000, "none"), (2, 30, 3000, "none")]
ROTATIONS = {  # key: (name, {category: weight}, transitions)
    "GECE":   ("Gece Seansı",            {"DEEP": 30, "HOUSE": 20, "EDM": 20, "POP": 10, "CLASSIC": 10, "TR": 10}, T_NIGHT),
    "SABAH":  ("Güne Merhaba",           {"TR": 35, "POP": 15, "CLASSIC": 15, "URBAN": 10, "HOUSE": 10, "EDM": 15}, T_POP),
    "OGLE":   ("Gün Ortası",             {"TR": 25, "POP": 15, "CLASSIC": 20, "HOUSE": 10, "DEEP": 10, "EDM": 20}, T_MIX),
    "HIT":    ("Hit Saatleri",           {"EDM": 35, "TR": 25, "POP": 10, "URBAN": 20, "HOUSE": 10}, T_MIX),
    "DONUS":  ("Eve Dönüş",              {"EDM": 45, "TR": 20, "URBAN": 15, "POP": 10, "HOUSE": 10}, T_MIX),
    "AKSAM":  ("Akşam Dansı",            {"EDM": 45, "HOUSE": 25, "URBAN": 10, "DEEP": 10, "MASHUP": 5, "POP": 5}, T_DANCE),
    "ISINMA": ("Dans Isınması",          {"EDM": 40, "HOUSE": 20, "URBAN": 15, "TR": 15, "MASHUP": 10}, T_DANCE),
    "KULUP":  ("Kulüp Gecesi",           {"EDM": 50, "HOUSE": 25, "MASHUP": 15, "URBAN": 10}, T_CLUB),
    "AFTER":  ("After Party",            {"EDM": 40, "HOUSE": 30, "DEEP": 20, "MASHUP": 10}, T_CLUB),
    "HSABAH": ("Hafta Sonu Sabahı",      {"CLASSIC": 30, "POP": 15, "TR": 25, "DEEP": 10, "EDM": 20}, T_SOFT),
    "CTSHIT": ("Cumartesi Hitleri",      {"EDM": 35, "TR": 25, "POP": 15, "URBAN": 25}, T_MIX),
    "RETRO":  ("Retro & Gold",           {"CLASSIC": 60, "POP": 20, "TR": 20}, T_POP),
    "DJSET":  ("DJ Set & Kulüp Gecesi",  {"DJSET": 30, "EDM": 40, "HOUSE": 20, "MASHUP": 10}, T_CLUB),
    "PSABAH": ("Pazar Keyfi",            {"DEEP": 25, "CLASSIC": 30, "POP": 15, "TR": 20, "EDM": 10}, T_SOFT),
    "PTR":    ("Pazar Türkçe Pop",       {"TR": 65, "POP": 20, "CLASSIC": 15}, T_POP),
    "PGOLD":  ("Pazar Klasikleri",       {"CLASSIC": 55, "POP": 25, "TR": 20}, T_POP),
    "KAPANIS":("Haftanın Kapanışı",      {"DEEP": 30, "HOUSE": 20, "EDM": 20, "POP": 15, "CLASSIC": 15}, T_NIGHT),
}
COLORS = {"GECE": "#3b4a6b", "SABAH": "#f2b134", "OGLE": "#6cc0a3", "HIT": "#e8644a", "DONUS": "#f08a3c",
          "AKSAM": "#8e5bd1", "ISINMA": "#b45bd1", "KULUP": "#d13b8e", "AFTER": "#6b3bd1", "HSABAH": "#f5d36b",
          "CTSHIT": "#e8644a", "RETRO": "#c9a227", "DJSET": "#c2185b", "PSABAH": "#9fd4c2", "PTR": "#e53935",
          "PGOLD": "#b8860b", "KAPANIS": "#455a8a"}
def H(h, m=0): return h * 60 + m
WEEKDAY = [(H(0), H(6), "GECE"), (H(6), H(10), "SABAH"), (H(10), H(14), "OGLE"), (H(14), H(18), "HIT"),
           (H(18), H(21), "DONUS"), (H(21), H(24), "AKSAM")]
FRIDAY = WEEKDAY[:4] + [(H(18), H(21), "ISINMA"), (H(21), H(24), "KULUP")]
SATURDAY = [(H(0), H(3), "AFTER"), (H(3), H(8), "GECE"), (H(8), H(12), "HSABAH"), (H(12), H(16), "CTSHIT"),
            (H(16), H(19), "RETRO"), (H(19), H(21), "ISINMA"), (H(21), H(24), "DJSET")]
SUNDAY = [(H(0), H(3), "AFTER"), (H(3), H(9), "GECE"), (H(9), H(12), "PSABAH"), (H(12), H(16), "PTR"),
          (H(16), H(20), "PGOLD"), (H(20), H(24), "KAPANIS")]
WEEK = [WEEKDAY, WEEKDAY, WEEKDAY, WEEKDAY, FRIDAY, SATURDAY, SUNDAY]  # Monday..Sunday

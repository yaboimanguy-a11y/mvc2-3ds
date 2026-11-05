#!/usr/bin/env python3
"""make_movelist.py SOURCE OUT: the 3DS app's move list (movelist.txt) from
port/platform/3ds/movelist_source.txt (the Marvel vs. Capcom Wiki's MvC2
move list, CC BY-SA).

Output, one block per character id (the game's character id, player record
byte 1 / +0x540, as port/host/arcade.c forces them):
    @1E Akuma
    Gohadoken|QCF+P
    ...
Inputs are compacted for the 40-column bottom screen: "QCF + P P" -> "QCF+PP",
"(air)" -> "j." (in the air only), "(air ok)" -> trailing "*" (also in the air).
"""
import re
import sys

# game character ids (0x18..0x1A are Abyss' forms: no list)
IDS = {
    "Ryu": 0x00, "Zangief": 0x01, "Guile": 0x02, "Morrigan": 0x03, "Anakaris": 0x04,
    "Strider Hiryu": 0x05, "Cyclops": 0x06, "Wolverine": 0x07, "Psylocke": 0x08, "Iceman": 0x09,
    "Rogue": 0x0A, "Captain America": 0x0B, "Spider-Man": 0x0C, "Hulk": 0x0D, "Venom": 0x0E,
    "Doctor Doom": 0x0F, "Tron": 0x10, "Jill": 0x11, "Hayato": 0x12, "Ruby Heart": 0x13,
    "SonSon": 0x14, "Amingo": 0x15, "Marrow": 0x16, "Cable": 0x17, "Chun-Li": 0x1B,
    "Mega Man/Rockman": 0x1C, "Roll": 0x1D, "Akuma/Gouki": 0x1E, "Baby Bonnie Hood/Bulleta": 0x1F,
    "Felicia": 0x20, "Charlie/Nash": 0x21, "Sakura": 0x22, "Dan": 0x23, "Cammy": 0x24,
    "Dhalsim": 0x25, "M. Bison/Vega": 0x26, "Ken": 0x27, "Gambit": 0x28, "Juggernaut": 0x29,
    "Storm": 0x2A, "Sabretooth": 0x2B, "Magneto": 0x2C, "Shuma-Gorath": 0x2D, "War Machine": 0x2E,
    "Silver Samurai": 0x2F, "Omega Red": 0x30, "Spiral": 0x31, "Colossus": 0x32, "Iron Man": 0x33,
    "Sentinel": 0x34, "Blackheart": 0x35, "Thanos": 0x36, "Jin": 0x37, "Captain Commando": 0x38,
    "Wolverine (Bone claw)": 0x39, "Servbot": 0x3A,
}
# form variants listed after their base character
FORMS = {"Dark Sakura": "Sakura", "Mech Zangief": "Zangief"}
SHORT = {"Akuma/Gouki": "Akuma", "Mega Man/Rockman": "Mega Man", "Baby Bonnie Hood/Bulleta": "B.B. Hood",
         "Charlie/Nash": "Charlie", "M. Bison/Vega": "M. Bison", "Wolverine (Bone claw)": "Wolverine (bone)",
         "Doctor Doom": "Dr. Doom"}
# corrections to the source (input as the game takes it)
FIX = {
    ("Sakura", "Hadoken"): "(air ok) QCF + P",
    ("Sakura", "Shouo'uken"): "(air ok) DP + P",
    ("Sakura", "Shinkuu Hadoken"): "QCF + P P",
    ("Sakura", "Midare Zakura"): "QCF + K K",
}


def compact(s):
    air_ok = "(air ok)" in s
    air = "(air)" in s
    s = s.replace("(air ok)", "").replace("(air)", "")
    s = s.replace("α", "alpha").replace("β", "beta").replace("γ", "gamma")
    s = s.replace("Assist 1 + Assist 2", "A1+A2").replace("Partner Assist", "assist")
    s = s.replace("(mash)", "mash")
    s = re.sub(r"\bP P\b", "PP", s)
    s = re.sub(r"\bK K\b", "KK", s)
    s = re.sub(r"\s*\+\s*", "+", s)
    s = re.sub(r"\s*,\s*", ", ", s)
    s = re.sub(r"\s+", " ", s).strip().rstrip(",").strip()
    if air:
        s = "j." + s
    if air_ok:
        s += "*"
    return s.encode("ascii", "replace").decode()


def main():
    src, out = sys.argv[1], sys.argv[2]
    blocks, cur = {}, None
    for line in open(src, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("## "):
            cur = line[3:].strip()
            blocks.setdefault(cur, [])
        elif cur and "|" in line and not line.startswith("#"):
            name, inp = (x.strip() for x in line.split("|", 1))
            base = name.lstrip("> ").strip()
            inp = FIX.get((cur, base), inp)
            blocks[cur].append((name.replace("> ", "> ").encode("ascii", "replace").decode(), compact(inp)))
    lines = [
        "# MvC2 move list for the 3DS app's bottom screen (mvc2.3dsx reads it from its folder).",
        "# From the Marvel vs. Capcom Wiki (Fandom), community content under CC BY-SA 3.0:",
        "# https://marvelvscapcom.fandom.com/wiki/Marvel_vs._Capcom_2:_New_Age_of_Heroes_move_list",
        "# Edit freely.  @ID Name starts a character (ID = the game's character id, hex);",
        "# then one move per line: Move name|Input.  Lines starting with # are ignored.",
        "# QCF D,DF,F  QCB D,DB,B  DP F,D,DF  RDP B,D,DB  HCF B,DB,D,DF,F  HCB F,DF,D,DB,B",
        "# [B]F hold back, then forward   j. = in the air   * = also in the air",
        "# P any punch  K any kick  PP / KK both  LP HP LK HK  A1 A2 assists",
        "",
    ]
    for name, cid in sorted(IDS.items(), key=lambda kv: kv[1]):
        if name not in blocks:
            sys.exit(f"make_movelist: no moves for {name}")
        lines.append(f"@{cid:02X} {SHORT.get(name, name)}")
        lines += [f"{n}|{i}" for n, i in blocks[name]]
        for form, base in FORMS.items():
            if base == name and form in blocks:
                lines.append(f"-- {form} --|")
                lines += [f"{n}|{i}" for n, i in blocks[form]]
        lines.append("")
    open(out, "w", encoding="ascii").write("\n".join(lines))


if __name__ == "__main__":
    main()

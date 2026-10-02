#!/usr/bin/env python3
"""
juggler_anim.py - Genera le 24 scene di un'animazione del Juggler.

Le scene originali dell'animazione di Eric Graham (1986-87) sono perse.
I tempi e il percorso dei palloni sono misurati sui 24 fotogrammi
originali (movie.data, juggler.avi di Ernie Wright): uno "shower", ogni
pallone fa un giro di 72 fotogrammi - lancio alto sopra la testa da una
mano all'altra in 47 fotogrammi (apice a meta'), poi un passaggio basso
davanti al petto, l'altra mano lo prende, lo porta giu' e lo rilancia.
I tre palloni sono sfasati di 24 fotogrammi, quindi la scena si ripete
ogni 24: da un lancio alto al successivo. Geometria e telecamera sono
quelle di robot.dat (la telecamera dell'animazione originale e' diversa e
non e' stata ricavata). Il robot sta fermo e piega le ginocchia solo
quando una mano riceve un pallone.

Uso: juggler_anim.py cartella_di_uscita   (scrive j00.dat ... j23.dat)
"""
import math
import os
import sys

N = 24                     # fotogrammi della scena (un terzo del giro)
CYCLE = 72                 # giro di un pallone
T = [-0.9, -2.1, 5.0]      # lancio alto (mano -1, a destra sullo schermo)
C = [-0.9, 1.9, 5.0]       # presa del lancio alto (mano +1)
M = [-1.3, -0.5, 5.6]      # presa del passaggio basso (mano -1)
APEX = 2.4                 # altezza del lancio alto sopra T
HAND = 0.7                 # dal centro del pallone alla mano
# fasi del giro (fotogrammi): volo alto, mano +1, passaggio, mano -1
T_HIGH, T_HOLD1, T_PASS, T_HOLD2 = 47, 50, 62, 72


def lerp(a, b, t):
    return [a[k] + (b[k] - a[k]) * t for k in range(3)]


def ik(root, end, l1, l2, pole):
    """Giunto intermedio di una catena a due segmenti da root a end,
    piegato verso la direzione pole."""
    d = [end[k] - root[k] for k in range(3)]
    L = math.sqrt(sum(c * c for c in d))
    L = min(L, l1 + l2 - 1e-6)
    a = (l1 * l1 - l2 * l2 + L * L) / (2 * L)
    h = math.sqrt(max(l1 * l1 - a * a, 0.0))
    u = [c / L for c in d]
    p = [pole[k] - sum(pole[i] * u[i] for i in range(3)) * u[k] for k in range(3)]
    pl = math.sqrt(sum(c * c for c in p)) or 1.0
    p = [c / pl for c in p]
    return [root[k] + u[k] * a + p[k] * h for k in range(3)]


def ball(t):
    """Posizione di un pallone al tempo t del suo giro, e la mano che lo
    tiene (+1, -1 o 0 se e' in volo)."""
    t %= CYCLE
    if t < T_HIGH:                                  # lancio alto, parabola
        s = t / T_HIGH
        p = lerp(T, C, s)
        p[2] += 4 * APEX * s * (1 - s)
        return p, 0
    if t < T_HOLD1:                                 # presa: la mano cede un po'
        s = (t - T_HIGH) / (T_HOLD1 - T_HIGH)
        return [C[0], C[1], C[2] - 0.15 * math.sin(math.pi * s)], 1
    if t < T_PASS:                                  # passaggio basso
        s = (t - T_HOLD1) / (T_PASS - T_HOLD1)
        p = lerp(C, M, s)
        p[2] += 4 * 0.5 * s * (1 - s)
        return p, 0
    s = (t - T_PASS) / (T_HOLD2 - T_PASS)           # presa, giu' e rilancio
    p = lerp(M, T, s)
    p[2] -= 0.3 * math.sin(math.pi * s)
    return p, -1


def hand(f, side):
    """Mano side al fotogramma f: sotto il pallone che tiene, altrimenti
    dal punto di rilascio verso la prossima presa."""
    for k in range(3):
        p, h = ball(f + N * k)
        if h == side:
            return [p[0], p[1], p[2] - HAND]
    # mano vuota: rilascio e presa successiva nel giro, ripetuti ogni N
    rel, cat = (T_HOLD1, T_HIGH + N) if side > 0 else (T_HOLD2, T_PASS + N)
    t = f % N
    while t < rel % N:
        t += N
    t0 = rel % N
    while t < t0:
        t += N
    s = (t - t0) / (cat - rel)
    a = ball(rel - 1e-6)[0]
    b = ball(cat - N)[0]
    p = lerp(a, b, s)
    p[2] -= 0.25 * math.sin(math.pi * s) + HAND
    return p


def knees(f):
    """Molleggio: le ginocchia si piegano alle prese e tornano dritte."""
    dips = (-0.08, -0.06, -0.03, -0.01)
    best = 0.0
    for c in (T_HIGH % N, T_PASS % N):
        d = (f - c) % N
        if d < len(dips):
            best = min(best, dips[d])
    return best


def frame(f):
    dx = 0.0
    bob = knees(f)
    sp = []                                           # (colore, tipo, catena)

    def mv(p, z=0.0):
        return [p[0] + dx, p[1], p[2] + z]

    # i tre palloni, sfasati di un terzo del giro
    balls = [ball(f + N * k)[0] for k in range(3)]
    hands = {side: hand(f, side) for side in (1, -1)}
    for b in balls:
        sp.append(("<.9,.9,.9>", 2, [(mv(b), 0.6)]))
    # testa, faccia, occhi, collo, corpo (come robot.dat, con il su e giu')
    sp.append(("<1,.7,.7>", 1, [(mv([0, 0, 6.1], bob), 0.5)]))
    sp.append(("<.2,.1,.1>", 1, [(mv([0.02, 0, 6.12], bob), 0.5)]))
    sp.append(("<.1,.1,1.>", 1, [(mv([-0.4, 0.2, 6.1], bob), 0.15)]))
    sp.append(("<.1,.1,1.>", 1, [(mv([-0.4, -0.2, 6.1], bob), 0.15)]))
    sp.append(("<1,.7,.7>", 1, [(mv([0, 0, 5.5], bob), 0.2)]))
    sp.append(("<1,.1,.1>", 1, [(mv([0, 0, 4.6], bob), 0.8), 5, (mv([0, 0, 3.3], bob), 0.6)]))
    # gambe: piedi fermi, il ginocchio si piega con il molleggio
    for hip, foot, l1, l2 in (([0, 0.6, 2.9], [-0.4, 0.6, 0], 1.432, 1.612),
                              ([0, -0.6, 2.9], [0.4, -0.6, 0], 1.315, 1.612)):
        hip = [hip[0], hip[1], hip[2] + bob]
        knee = ik(hip, foot, l1, l2, [-1, 0, 0])
        sp.append(("<1,.7,.7>", 1, [(mv(hip), 0.2), 6, (mv(knee), 0.2), 7, (mv(foot), 0.1)]))
    # braccia: spalla, gomito (IK, verso il basso e in fuori), mano
    for side in (1, -1):
        sh = [0, side * 0.7, 5.1 + bob]
        hand = hands[side]
        elbow = ik(sh, hand, 1.05, 1.21, [0.3, side * 1.0, -1.0])
        sp.append(("<1,.7,.7>", 1, [(mv(sh), 0.2), 6, (mv(elbow), 0.2), 7, (mv(hand), 0.1)]))
    return dx, sp


def fmt(v):
    return "(%s)" % ",".join(("%.3f" % c).rstrip("0").rstrip(".") for c in v)


def write(path, f):
    dx, sp = frame(f)
    lines = [fmt([-10 + dx, -4, 5.5]), "[-10,20]", "35", ""]
    for col, ty, chain in sp:
        s = "  %s %d  %s:%s" % (col, ty, fmt(chain[0][0]), chain[0][1])
        i = 1
        while i < len(chain):
            n, (p, r) = chain[i], chain[i + 1]
            s += " %d %s:%s" % (n, fmt(p), r)
            i += 2
        lines.append(s + ";")
    lines[-1] += ";"
    lines += ["", "1", "   %s:15 <1,1,1>" % fmt([-100, 50, 150]), "",
              "<1.5,1.5,0>  <0,1.5,0> <.25,.25,.25> <.1,.1,1> <.7,.7,1>", ""]
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for f in range(N):
        write(os.path.join(out, "j%02d.dat" % f), f)

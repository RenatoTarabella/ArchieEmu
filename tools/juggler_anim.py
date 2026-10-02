#!/usr/bin/env python3
"""
juggler_anim.py - Genera le 24 scene di un'animazione del Juggler.

Le scene originali dell'animazione di Eric Graham (1986-87) sono perse:
questa e' una ricostruzione procedurale nello stesso spirito, a partire
dalla geometria di robot.dat. Il robot cammina verso l'osservatore (la
telecamera e la lampada lo seguono, il pavimento scorre di 6 unita' in
24 fotogrammi, cosi' il ciclo si chiude) e fa giocoleria a cascata con
tre palloni a specchio: un lancio ogni 4 fotogrammi, mani alternate,
8 fotogrammi in volo e 4 in mano.

Uso: juggler_anim.py cartella_di_uscita   (scrive j00.dat ... j23.dat)
"""
import math
import os
import sys

N = 24                     # fotogrammi
STEP = 6.0 / N             # avanzamento per fotogramma (verso -x)
X0 = 30.0                  # tutto spostato in x>0: scacchi periodici
G = 0.25                   # gravita' in unita'/fotogramma^2
BALL_X = -1.2              # piano dei palloni, davanti al corpo
CATCH_Y, THROW_Y, HAND_Z = 2.1, 1.3, 4.8   # centro del pallone in mano


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


def ball_and_hand(f, side):
    """Pallone tenuto dalla mano side (+1 sinistra, -1 destra) al
    fotogramma f, o None, e posizione della mano.
    La destra lancia a 0, 8, 16 e prende a 4, 12, 20; la sinistra lancia
    a 4, 12, 20 e prende a 0, 8, 16."""
    c = (f - (4 if side < 0 else 0)) % 8      # 0 = presa, 4 = lancio
    if c <= 4:                                 # in mano: dalla presa al lancio
        s = c / 4.0
        y = side * (CATCH_Y + (THROW_Y - CATCH_Y) * s)
        z = HAND_Z - 0.4 * math.sin(math.pi * s)
        ball = [BALL_X, y, z]
        return ball, [BALL_X, y, z - 0.7]
    s = (c - 4) / 4.0                          # vuota: dal lancio alla presa
    y = side * (THROW_Y + (CATCH_Y - THROW_Y) * s)
    return None, [BALL_X, y, HAND_Z - 0.7 + 0.25 * math.sin(math.pi * s)]


def flying(f):
    """Palloni in volo: lanciati da una mano verso l'altra, 8 fotogrammi."""
    out = []
    for k in range(6):                         # lanci a 0,4,...,20
        t0 = 4 * k
        t = (f - t0) % N
        if 0 < t < 8:
            side = -1 if t0 % 8 == 0 else 1    # chi lancia
            y0, y1 = side * THROW_Y, -side * CATCH_Y
            y = y0 + (y1 - y0) * t / 8.0
            z = HAND_Z + (G * 8 / 2) * t - G * t * t / 2
            out.append([BALL_X, y, z])
    return out


def frame(f):
    dx = X0 - STEP * f
    bob = -0.08 * math.cos(2 * math.pi * f / 6)      # due volte per passo
    sp = []                                           # (colore, tipo, catena)

    def mv(p, z=0.0):
        return [p[0] + dx, p[1], p[2] + z]

    # palloni: quelli in mano e quelli in volo
    balls = []
    hands = {}
    for side in (1, -1):
        b, h = ball_and_hand(f, side)
        hands[side] = h
        if b:
            balls.append(b)
    balls += flying(f)
    for b in balls:
        sp.append(("<.9,.9,.9>", 2, [(mv(b), 0.6)]))
    # testa, faccia, occhi, collo, corpo (come robot.dat, con il su e giu')
    sp.append(("<1,.7,.7>", 1, [(mv([0, 0, 6.1], bob), 0.5)]))
    sp.append(("<.2,.1,.1>", 1, [(mv([0.02, 0, 6.12], bob), 0.5)]))
    sp.append(("<.1,.1,1.>", 1, [(mv([-0.4, 0.2, 6.1], bob), 0.15)]))
    sp.append(("<.1,.1,1.>", 1, [(mv([-0.4, -0.2, 6.1], bob), 0.15)]))
    sp.append(("<1,.7,.7>", 1, [(mv([0, 0, 5.5], bob), 0.2)]))
    sp.append(("<1,.1,.1>", 1, [(mv([0, 0, 4.6], bob), 0.8), 5, (mv([0, 0, 3.3], bob), 0.6)]))
    # gambe: piede d'appoggio che scorre all'indietro, l'altro in volo
    for side, ph in ((1, 0.0), (-1, 0.5)):
        hip = [0, side * 0.6, 2.9 + bob]
        s = (f / 12.0 + ph) % 1.0                  # due passi per gamba in 24
        if s < 0.5:                                 # appoggio: da -0.75 a +0.75
            fx, fz = -0.75 + 3.0 * s, 0.0
        else:                                       # volo: torna avanti
            q = (s - 0.5) * 2
            fx, fz = 0.75 - 1.5 * q, 0.35 * math.sin(math.pi * q)
        foot = [fx, side * 0.6, fz]
        knee = ik(hip, foot, 1.43, 1.61, [-1, 0, 0])
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
    lines += ["", "1", "   %s:15 <1,1,1>" % fmt([-100 + dx, 50, 150]), "",
              "<1.5,1.5,0>  <0,1.5,0> <.25,.25,.25> <.1,.1,1> <.7,.7,1>", ""]
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for f in range(N):
        write(os.path.join(out, "j%02d.dat" % f), f)

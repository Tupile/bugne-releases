"""
Boitier ENFANT "robot rigolo" pour la carte LCDWIKI ES3C28P.

La grille HP est le SOURIRE du robot (plaque stade sombre percee), les yeux
sont des inlays 3e couleur, l'ecran est le ventre. Poignee-arche entre deux
antennes sur le crane. Coeur mecanique repris de es3c28p-braun-stl.py.
Carte en paysage, HP au-dessus (cable court), USB a droite. Pas de pieds
(piece arrachable = avalable), fond plat, gros conges partout.
Imprime FACE CONTRE LE PLATEAU, aucun support. PETG conseille (poignee).

Repere : X = largeur, Y = profondeur (facade Y=0, arriere +Y), Z = hauteur.
Lancer : python3 es3c28p-robot-stl.py
"""
import cadquery as cq

# ---- Cotes carte ----
PCB_W, PCB_H, PCB_T = 86.0, 50.0, 1.6
HOLE_DX, HOLE_DZ = 39.0, 21.0
WIN_W, WIN_H = 60.5, 46.5
SPK_W, SPK_H, SPK_T = 41.0, 28.5, 9.15

# ---- Coque (largeur asymetrique comme le braun, tete plus haute) ----
FRONT_T = 2.5
WALL = 2.0
LID_T = 4.0                    # 4 mm : fraisage profond, vis sous la surface
X_LEFT = -50.0
X_RIGHT = 45.5
CAB_W = X_RIGHT - X_LEFT       # 95.5
CAB_CX = (X_LEFT + X_RIGHT) / 2.0   # -2.25
CAB_H = 110.0                  # front haut : place pour les yeux
DEPTH = 32.0
R_CORNER = 6.0                 # gros conges enfant
FRONT_R = 2.0                  # roundover du perimetre de facade

# ---- Fenetre ecran (valide braun) ----
WIN_OPEN_W = WIN_W + 3.0
WIN_CX = CAB_CX
WIN_CH = 3.0
Z_BOARD = 33.0
PCB_FRONT = FRONT_T + 4.3
PCB_BACK = PCB_FRONT + PCB_T
STANDOFF_R = 3.5
BOARD_PILOT = 1.25

# ---- HP : tablette + nervures = guidage seulement ; la retenue arriere est un
# plot solidaire du capot qui appuie sur l'aimant (les nervures s'arretent avant
# le dos du HP, sans le plot il bascule derriere l'ecran) ----
SHELF_Z, SHELF_T, SHELF_DEPTH = 60.0, 2.0, 13.0
SPK_CLR, RIBT, RIBD = 0.5, 2.0, 9.0
HP_Z = SHELF_Z + SHELF_T + SPK_H / 2.0      # 76.25
HP_STOP_W, HP_STOP_H, HP_STOP_CLR = 14.0, 16.0, 0.25   # plot de butee (sur le capot)
HP_WIRE_H, HP_WIRE_D = 6.0, 5.0             # canal cable dans le bout du plot
SND_AX, SND_AZ = 20.5, 13.0

# ---- Sourire-grille (2e couleur) : plaque STADE dans une feuillure facade ----
PLATE_T = 1.4
SMILE_W, SMILE_H = 74.0, 32.0  # contient l'ellipse son + 2 mm mini partout
SMILE_Z0 = HP_Z - SMILE_H / 2.0             # 60.25
HOLE_D, HOLE_PITCH = 2.4, 4.5
BLIND_DEPTH = 1.0
HOLE_EDGE = 2.0

# ---- Yeux (3e couleur) + pupilles (couleur du sourire) ----
EYE_R, EYE_DX, EYE_Z = 6.0, 22.0, 100.5
PUPIL_R, PUPIL_OFF_X, PUPIL_OFF_Z = 2.5, 1.2, -0.5  # regard vers le centre, bas

# ---- Poignee-arche + antennes sur le crane ----
ARCH_W, ARCH_H, ARCH_D = 64.0, 26.0, 16.0   # bande pleine, moins profonde
ARCH_EMBED = 6.0               # ancrage sous le sommet du crane
SLOT_W, SLOT_H = 48.0, 16.0    # fente de prise stade (extremites R8)
ROOT_R = 5.0                   # conges concaves aux racines
RIM_R = 2.5                    # adoucissement des rives avant/arriere
EAR_W, EAR_H, EAR_R = 10.0, 16.0, 4.0
EAR_DX = 40.0                  # antennes de part et d'autre de l'arche

# ---- USB sur le flanc droit (valide braun) ----
USB_Y = 10.0
USB_OPEN_W, USB_OPEN_H, USB_OPEN_R = 10.5, 6.0, 1.0

# ---- Capot arriere plat (vis sous la surface) ----
CAP_PTS = [(-45.5, 12.0), (41.0, 12.0), (-45.5, 100.0), (41.0, 100.0)]
CAP_BOSS_R = 3.5
CAP_BOSS_LEN = 11.0
CAP_PILOT = 1.35
CAP_SCREW_CLR = 1.8
CAP_HEAD_R = 3.3
CAP_HEAD_D = 2.6               # tete M3 bombee (1.9-2.5) 0.5 sous la surface
LID_GAP = 0.2
LID_CH = 1.2                   # chanfrein du perimetre arriere du capot
LIP_T, LIP_H, LIP_LEN, LIP_CLR = 1.6, 1.8, 36.0, 0.25
BOSS_BACK = DEPTH - LID_T      # 28


def ycyl(r, y0, y1, x, z):
    c = cq.Workplane("XY").circle(r).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90)
    return c.translate((x, y0, z))


def yell(ax, az, y0, y1, cx, cz):
    e = cq.Workplane("XY").ellipse(ax, az).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90)
    return e.translate((cx, y0, cz))


def yholes(pts, r, y0, y1):
    """Paquet de cylindres le long de Y aux centres (x, z) : un seul booleen."""
    c = (cq.Workplane("XY").pushPoints([(x, -z) for (x, z) in pts])
         .circle(r).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90))
    return c.translate((0, y0, 0))


def ystadium(w, h, y0, y1, cx, cz):
    """Stade (rectangle + extremites demi-rondes) le long de Y."""
    s = (cq.Workplane("XY").box(w - h, y1 - y0, h, centered=(True, False, False))
         .union(ycyl(h / 2, 0, y1 - y0, -(w - h) / 2, h / 2))
         .union(ycyl(h / 2, 0, y1 - y0, (w - h) / 2, h / 2)))
    return s.translate((cx, y0, cz - h / 2))


def sf(solid, sel, r):
    try:
        return solid.edges(sel).fillet(r)
    except Exception as e:
        print("  (fillet ignore %s r=%s : %s)" % (sel, r, e))
        return solid


def in_stadium(dx, dz, w, h):
    """Le point (dx, dz) est-il dans le stade w x h centre a l'origine."""
    dxe = max(0.0, abs(dx) - (w - h) / 2.0)
    return dxe * dxe + dz * dz <= (h / 2.0) ** 2


# ---- Trame de trous du sourire : marge testee contre le STADE retracte ----
def hole_grid():
    inset = HOLE_EDGE + HOLE_D / 2.0
    wi, hi = SMILE_W - 2 * inset, SMILE_H - 2 * inset
    blind, thru = [], []
    n = int(SMILE_W // HOLE_PITCH)
    for i in range(-n, n + 1):
        for j in range(-n, n + 1):
            dx, dz = i * HOLE_PITCH, j * HOLE_PITCH
            if not in_stadium(dx, dz, wi, hi):
                continue
            ax, az = SND_AX - HOLE_D / 2, SND_AZ - HOLE_D / 2
            if (dx / ax) ** 2 + (dz / az) ** 2 <= 1.0:
                thru.append((CAB_CX + dx, HP_Z + dz))
            else:
                blind.append((CAB_CX + dx, HP_Z + dz))
    return blind, thru


PTS_BLIND, PTS_THRU = hole_grid()

# Plaque sourire : le MEME solide sert de feuillure dans le corps
smile = ystadium(SMILE_W, SMILE_H, 0, PLATE_T, CAB_CX, HP_Z)

# Yeux : disques pleins pour la feuillure ; anneau (blanc) + pupille (sombre)
EYES = [(CAB_CX - EYE_DX, EYE_Z, +1), (CAB_CX + EYE_DX, EYE_Z, -1)]
eye_rebate = None
eyes_part = None
pupils = None
for (ex, ez, look) in EYES:
    disc = ycyl(EYE_R, 0, PLATE_T, ex, ez)
    pup = ycyl(PUPIL_R, -0.1, PLATE_T + 0.1, ex + look * PUPIL_OFF_X, ez + PUPIL_OFF_Z)
    ring = disc.cut(pup)
    pup = ycyl(PUPIL_R, 0, PLATE_T, ex + look * PUPIL_OFF_X, ez + PUPIL_OFF_Z)
    eye_rebate = disc if eye_rebate is None else eye_rebate.union(disc)
    eyes_part = ring if eyes_part is None else eyes_part.union(ring)
    pupils = pup if pupils is None else pupils.union(pup)

# ============================================================================
# 1. Corps : boite droite haute, ouverte a l'arriere
# ============================================================================
body = (cq.Workplane("XY")
        .box(CAB_W, DEPTH, CAB_H, centered=(True, False, False))
        .edges("|Y").fillet(R_CORNER)
        .translate((CAB_CX, 0, 0)))
try:
    body = body.faces("<Y").edges().fillet(FRONT_R)
except Exception as e:
    print("  (roundover facade ignore : %s)" % e)

cavity = (cq.Workplane("XY")
          .box(CAB_W - 2 * WALL, DEPTH + 10, CAB_H - 2 * WALL, centered=(True, False, False))
          .edges("|Y").fillet(R_CORNER - WALL / 2)
          .translate((CAB_CX, FRONT_T, WALL)))
body = body.cut(cavity)

# Fenetre ecran chanfreinee (valide braun)
body = body.cut(cq.Workplane("XY").box(WIN_OPEN_W, FRONT_T + 4, WIN_H, centered=(True, True, True))
                .translate((WIN_CX, FRONT_T / 2.0, Z_BOARD)))
cham = (cq.Workplane("XY").rect(WIN_OPEN_W + 2 * WIN_CH, WIN_H + 2 * WIN_CH)
        .workplane(offset=WIN_CH).rect(WIN_OPEN_W, WIN_H).loft())
cham = cham.rotate((0, 0, 0), (1, 0, 0), -90).translate((WIN_CX, 0, Z_BOARD))
body = body.cut(cham)

# Tablette + nervures HP, encoche fils (valide braun)
body = body.union(cq.Workplane("XY").box(CAB_W - 2 * WALL + 1, SHELF_DEPTH, SHELF_T, centered=(True, False, False))
                  .translate((CAB_CX, FRONT_T - 1, SHELF_Z)))
z_bot = SHELF_Z + SHELF_T
xr = SPK_W / 2.0 + SPK_CLR
z_top = z_bot + SPK_H + SPK_CLR
z_mid = z_bot + SPK_H / 2.0


def rib(w, h, x, z):
    return (cq.Workplane("XY").box(w, RIBD, h, centered=(True, False, False))
            .translate((x, FRONT_T - 1, z)))


body = body.union(rib(RIBT, SPK_H + 2 * SPK_CLR, CAB_CX - xr - RIBT / 2, z_bot - SPK_CLR))
rr = rib(RIBT, SPK_H + 2 * SPK_CLR, CAB_CX + xr + RIBT / 2, z_bot - SPK_CLR)
rr = rr.cut(cq.Workplane("XY").box(RIBT + 2, RIBD + 4, 7.0, centered=(True, True, True))
            .translate((CAB_CX + xr + RIBT / 2, FRONT_T - 1 + RIBD / 2, z_mid)))
body = body.union(rr)
body = body.union(rib(2 * xr + 2 * RIBT, RIBT, CAB_CX, z_top))

# Feuillures : sourire, yeux ; puis ouverture son traversante
body = body.cut(smile)
body = body.cut(eye_rebate)
body = body.cut(yell(SND_AX, SND_AZ, -1, FRONT_T + 2, CAB_CX, HP_Z))

# Poignee-arche + antennes (unies APRES la cavite : l'ancrage plonge dedans).
# Rives adoucies piece par piece AVANT les unions (le fillet global echoue).
arch = (cq.Workplane("XY")
        .box(ARCH_W, ARCH_D, ARCH_H + ARCH_EMBED, centered=(True, False, False))
        .edges("|Y").fillet(10.0)
        .translate((CAB_CX, 0, CAB_H - ARCH_EMBED)))
arch = arch.cut(ystadium(SLOT_W, SLOT_H, -1, ARCH_D + 1, CAB_CX, CAB_H + SLOT_H / 2))
arch = sf(arch, "<Y", RIM_R)     # rives avant (fente comprise)
arch = sf(arch, ">Y", RIM_R)     # rives arriere
for s in (-1, 1):                # goussets concaves aux racines
    web = (cq.Workplane("XY")
           .box(ROOT_R, ARCH_D, ROOT_R, centered=(False, False, False))
           .translate((CAB_CX + (s * ARCH_W / 2 if s > 0 else -ARCH_W / 2 - ROOT_R), 0, CAB_H)))
    web = web.cut(ycyl(ROOT_R, -1, ARCH_D + 1, CAB_CX + s * (ARCH_W / 2 + ROOT_R), CAB_H + ROOT_R))
    arch = arch.union(web)
for s in (-1, 1):                # antennes
    ear = (cq.Workplane("XY")
           .box(EAR_W, ARCH_D, EAR_H, centered=(True, False, False))
           .edges("|Y").fillet(EAR_R))
    ear = sf(ear, "<Y", 2.0)
    ear = sf(ear, ">Y", 2.0)
    arch = arch.union(ear.translate((CAB_CX + s * EAR_DX, 0, CAB_H - EAR_H / 2)))
body = body.union(arch)

# Entretoises de montage carte
BOARD_PTS = [(-HOLE_DX, Z_BOARD - HOLE_DZ), (HOLE_DX, Z_BOARD - HOLE_DZ),
             (-HOLE_DX, Z_BOARD + HOLE_DZ), (HOLE_DX, Z_BOARD + HOLE_DZ)]
for (x, z) in BOARD_PTS:
    body = body.union(ycyl(STANDOFF_R, FRONT_T - 1, PCB_FRONT, x, z))
    body = body.cut(ycyl(BOARD_PILOT, FRONT_T + 1, PCB_FRONT + 0.5, x, z))

# Decoupe USB sur le flanc droit
usb = cq.Workplane("XY").box(2 * WALL + 6, USB_OPEN_H, USB_OPEN_W, centered=(True, True, True))
usb = sf(usb, "|X", USB_OPEN_R)
body = body.cut(usb.translate((X_RIGHT, USB_Y, Z_BOARD)))

# Bossages de vissage du capot (courts, derriere le PCB, ancres aux murs)
for (x, z) in CAP_PTS:
    body = body.union(ycyl(CAP_BOSS_R, BOSS_BACK - CAP_BOSS_LEN, BOSS_BACK, x, z))
    body = body.cut(ycyl(CAP_PILOT, BOSS_BACK - CAP_BOSS_LEN, BOSS_BACK + 0.1, x, z))

# ============================================================================
# 2. Sourire (2e couleur) : plaque percee + pupilles (meme couleur)
# ============================================================================
sourire = smile
if PTS_BLIND:
    sourire = sourire.cut(yholes(PTS_BLIND, HOLE_D / 2, -0.5, BLIND_DEPTH))
if PTS_THRU:
    sourire = sourire.cut(yholes(PTS_THRU, HOLE_D / 2, -0.5, PLATE_T + 0.5))
sourire = sourire.union(pupils)

# ============================================================================
# 3. Capot arriere plat (4 vis fraisees profond, segments de levre)
# ============================================================================
capot = (cq.Workplane("XY")
         .box(CAB_W - 2 * LID_GAP, LID_T, CAB_H - 2 * LID_GAP, centered=(True, False, False))
         .edges("|Y").fillet(R_CORNER - LID_GAP)
         .translate((CAB_CX, BOSS_BACK, LID_GAP)))
try:
    capot = capot.faces(">Y").edges().chamfer(LID_CH)
except Exception as e:
    print("  (chanfrein capot ignore : %s)" % e)
CAV_XL, CAV_XR = X_LEFT + WALL, X_RIGHT - WALL
CAV_ZB, CAV_ZT = WALL, CAB_H - WALL
lips = [
    (LIP_LEN, LIP_T, CAB_CX, CAV_ZB + LIP_CLR),
    (LIP_LEN, LIP_T, CAB_CX, CAV_ZT - LIP_CLR - LIP_T),
    (LIP_T, LIP_LEN, CAV_XL + LIP_CLR + LIP_T / 2, CAB_H / 2 - LIP_LEN / 2),
    (LIP_T, LIP_LEN, CAV_XR - LIP_CLR - LIP_T / 2, CAB_H / 2 - LIP_LEN / 2),
]
for (w, h, x, z) in lips:
    capot = capot.union(cq.Workplane("XY").box(w, LIP_H, h, centered=(True, False, False))
                        .translate((x, BOSS_BACK - LIP_H, z)))
for (x, z) in CAP_PTS:
    capot = capot.cut(ycyl(CAP_SCREW_CLR, BOSS_BACK - LIP_H - 1, DEPTH + 1, x, z))
    capot = capot.cut(ycyl(CAP_HEAD_R, DEPTH - CAP_HEAD_D, DEPTH + 1, x, z))

# Plot de butee HP : appuie sur l'aimant (jeu HP_STOP_CLR), canal horizontal a
# mi-hauteur dans le bout pour laisser passer le cable derriere l'aimant
STOP_Y0 = FRONT_T + SPK_T + HP_STOP_CLR
capot = capot.union(cq.Workplane("XY")
                    .box(HP_STOP_W, BOSS_BACK - STOP_Y0, HP_STOP_H, centered=(True, False, False))
                    .translate((CAB_CX, STOP_Y0, HP_Z - HP_STOP_H / 2)))
capot = capot.cut(cq.Workplane("XY")
                  .box(HP_STOP_W + 2, HP_WIRE_D + 1, HP_WIRE_H, centered=(True, False, False))
                  .translate((CAB_CX, STOP_Y0 - 1, HP_Z - HP_WIRE_H / 2)))

# ============================================================================
# Controles geometriques
# ============================================================================
assert PCB_BACK < BOSS_BACK - CAP_BOSS_LEN, "bossages capot vs PCB"
assert FRONT_T + SPK_T < BOSS_BACK, "profondeur HP"
assert SMILE_Z0 > Z_BOARD + WIN_H / 2 + WIN_CH, "sourire vs chanfrein ecran"
assert SND_AX + 2 <= SMILE_W / 2 and SND_AZ + 2 <= SMILE_H / 2, "ellipse son + 2 dans le sourire"
assert EYE_Z - EYE_R >= HP_Z + SMILE_H / 2 + 2, "yeux vs haut du sourire"
assert EYE_Z + EYE_R <= CAB_H - WALL - 1, "yeux vs sommet du crane"
assert EYE_R - (PUPIL_R + max(abs(PUPIL_OFF_X), abs(PUPIL_OFF_Z))) >= 1.5, "anneau des yeux"
assert ARCH_H - SLOT_H >= 10, "barre de prise >= 10"
assert (ARCH_W - SLOT_W) / 2 >= 6, "jambes de l'arche"
assert HP_Z + SND_AZ < z_top, "ouverture son vs nervure haute"
assert HP_STOP_W / 2 + 1 < xr, "plot capot vs nervures laterales"
assert HP_Z + HP_STOP_H / 2 < z_top, "plot capot vs nervure haute"
assert SHELF_Z + SHELF_T < HP_Z - HP_STOP_H / 2, "plot capot vs tablette"

# ============================================================================
# Export
# ============================================================================
parts = {
    "es3c28p_robot_corps.stl": body,
    "es3c28p_robot_capot.stl": capot,
    "es3c28p_robot_sourire.stl": sourire,
    "es3c28p_robot_yeux.stl": eyes_part,
}
for name, part in parts.items():
    cq.exporters.export(part, "./" + name)
    bb = part.val().BoundingBox()
    print("%-27s  X[%6.1f %6.1f]  Y[%6.1f %6.1f]  Z[%6.1f %6.1f]" % (
        name, bb.xmin, bb.xmax, bb.ymin, bb.ymax, bb.zmin, bb.zmax))

# STEP combine : corps + sourire (avec pupilles) + yeux, positions monde et
# couleurs conservees -> import direct multi-couleur dans Bambu/Prusa.
asm = cq.Assembly()
asm.add(body, name="corps", color=cq.Color(0.95, 0.78, 0.25))
asm.add(sourire, name="sourire", color=cq.Color(0.15, 0.15, 0.15))
asm.add(eyes_part, name="yeux", color=cq.Color(0.97, 0.97, 0.95))
asm.export("es3c28p_robot_corps+deco.step")
print("es3c28p_robot_corps+deco.step  (corps+sourire+yeux, 3 couleurs)")
print("tete %.1f x %.1f x %.1f (+arche %.0f), sourire : %d borgnes + %d traversants"
      % (CAB_W, DEPTH, CAB_H, ARCH_H, len(PTS_BLIND), len(PTS_THRU)))
print("impression : PETG conseille, elephant foot ON, >= 4 perimetres (arche)")

"""
Cabinet "seventies", inspire des designs des annees 70, pour la carte LCDWIKI ES3C28P.

Forme : boite droite epuree, facade plate, capot arriere plat visse.
Facade basse = fenetre ecran chanfreinee ; bande haute = plaque sombre a
trame reguliere de trous ronds (borgnes partout, traversants au droit du HP).
Carte en paysage, HP au-dessus de la carte (cable court), USB a droite.
Imprime FACE CONTRE LE PLATEAU (facade en bas), aucun support.

Repere : X = largeur, Y = profondeur (facade Y=0, arriere +Y), Z = hauteur.
Lancer : python3 es3c28p-seventies-stl.py
"""
import cadquery as cq

# ---- Cotes carte ----
PCB_W, PCB_H, PCB_T = 86.0, 50.0, 1.6
HOLE_DX, HOLE_DZ = 39.0, 21.0
WIN_W, WIN_H = 60.5, 46.5
SPK_W, SPK_H, SPK_T = 41.0, 28.5, 9.15

# ---- Coque (largeur asymetrique : ecran centre sur la facade, USB au bord droit) ----
FRONT_T = 2.5
WALL = 2.0
LID_T = 3.0
X_LEFT = -50.0                 # facade elargie a gauche (recentre l'ecran decale)
X_RIGHT = 45.5                 # bord droit : 0.5 de jeu entre carte et mur (carte a X=43)
CAB_W = X_RIGHT - X_LEFT       # 95.5
CAB_CX = (X_LEFT + X_RIGHT) / 2.0   # -2.25 : centre de la facade
CAB_H = 102.0
DEPTH = 32.0
R_CORNER = 3.5                 # conges des 4 coins du contour
FRONT_CH = 0.5                 # chanfrein du perimetre de la facade

# ---- Fenetre ecran (details valides du boitier radio precedent) ----
WIN_OPEN_W = WIN_W + 3.0       # ouverture elargie (ecran decale sur la carte)
WIN_CX = CAB_CX
WIN_CH = 3.0                   # chanfrein en pente autour de l'ecran
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
HP_Z = SHELF_Z + SHELF_T + SPK_H / 2.0      # 76.25 : centre membrane
HP_STOP_W, HP_STOP_H, HP_STOP_CLR = 14.0, 16.0, 0.25   # plot de butee (sur le capot)
HP_WIRE_H, HP_WIRE_D = 6.0, 5.0             # canal cable dans le bout du plot
SND_AX, SND_AZ = 20.5, 13.0    # ouverture son elliptique dans la facade

# ---- Trou micro (position reprise du boitier de base, approximative) ----
MIC_X, MIC_Z = -38.5, 48.0     # decale de 2.5 a gauche du micro : evite le chanfrein fenetre
MIC_R = 1.0

# ---- Plaque grille (2e couleur, fusionnee dans une feuillure de la facade) ----
PLATE_T = 1.4                  # feuillure et plaque : memes solides, coincidents
PLATE_INSET = 2.5              # lisere couleur corps autour de la plaque
PLATE_R = 2.0
PLATE_Z0, PLATE_Z1 = 63.0, 99.5
HOLE_D, HOLE_PITCH = 2.4, 4.5
BLIND_DEPTH = 1.0              # trous borgnes DANS la plaque : fond couleur plaque
HOLE_EDGE = 2.0                # marge mini trou -> bord de plaque

# ---- USB sur le flanc droit ----
USB_Y = 10.0
USB_OPEN_W, USB_OPEN_H, USB_OPEN_R = 10.5, 6.0, 1.0

# ---- Capot arriere plat ----
CAP_PTS = [(-45.5, 9.0), (41.0, 9.0), (-45.5, 93.0), (41.0, 93.0)]
CAP_BOSS_R = 3.5
CAP_BOSS_LEN = 11.0            # bossages courts : entierement derriere le PCB
CAP_PILOT = 1.35
CAP_SCREW_CLR = 1.8
CAP_HEAD_R = 3.3
CAP_HEAD_D = 1.6
LID_GAP = 0.2                  # retrait du capot par cote
LIP_T, LIP_H, LIP_LEN, LIP_CLR = 1.6, 1.8, 36.0, 0.25


def ycyl(r, y0, y1, x, z):
    c = cq.Workplane("XY").circle(r).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90)
    return c.translate((x, y0, z))


def yell(ax, az, y0, y1, cx, cz):
    """Creux elliptique (ax en X, az en Z), le long de Y de y0 a y1."""
    e = cq.Workplane("XY").ellipse(ax, az).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90)
    return e.translate((cx, y0, cz))


def yholes(pts, r, y0, y1):
    """Paquet de cylindres le long de Y aux centres (x, z) : un seul booleen."""
    c = (cq.Workplane("XY").pushPoints([(x, -z) for (x, z) in pts])
         .circle(r).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90))
    return c.translate((0, y0, 0))


def sf(solid, sel, r):
    try:
        return solid.edges(sel).fillet(r)
    except Exception as e:
        print("  (fillet ignore %s r=%s : %s)" % (sel, r, e))
        return solid


# ---- Trame de trous : grille reguliere centree, statut borgne/traversant ----
def hole_grid():
    xmin, xmax = X_LEFT + PLATE_INSET + HOLE_EDGE + HOLE_D / 2, X_RIGHT - PLATE_INSET - HOLE_EDGE - HOLE_D / 2
    zmin, zmax = PLATE_Z0 + HOLE_EDGE + HOLE_D / 2, PLATE_Z1 - HOLE_EDGE - HOLE_D / 2
    zc = (PLATE_Z0 + PLATE_Z1) / 2.0
    nx = int(min(CAB_CX - xmin, xmax - CAB_CX) // HOLE_PITCH)
    nz = int(min(zc - zmin, zmax - zc) // HOLE_PITCH)
    blind, thru = [], []
    for i in range(-nx, nx + 1):
        for j in range(-nz, nz + 1):
            hx, hz = CAB_CX + i * HOLE_PITCH, zc + j * HOLE_PITCH
            # traversant seulement si ENTIEREMENT dans l'ouverture son
            # (sinon le fond montrerait la facade couleur corps)
            ax, az = SND_AX - HOLE_D / 2, SND_AZ - HOLE_D / 2
            if ((hx - CAB_CX) / ax) ** 2 + ((hz - HP_Z) / az) ** 2 <= 1.0:
                thru.append((hx, hz))
            else:
                blind.append((hx, hz))
    return blind, thru


PTS_BLIND, PTS_THRU = hole_grid()

# Plaque grille : le MEME solide sert de feuillure dans le corps (coincidence
# exacte, aucun jeu : les deux pieces se soudent au slicing multi-couleur)
PLATE_W = CAB_W - 2 * PLATE_INSET
PLATE_H = PLATE_Z1 - PLATE_Z0
plate = (cq.Workplane("XY")
         .box(PLATE_W, PLATE_T, PLATE_H, centered=(True, False, False))
         .edges("|Y").fillet(PLATE_R)
         .translate((CAB_CX, 0, PLATE_Z0)))

# ============================================================================
# 1. Corps : boite droite, ouverte a l'arriere
# ============================================================================
body = (cq.Workplane("XY")
        .box(CAB_W, DEPTH, CAB_H, centered=(True, False, False))
        .edges("|Y").fillet(R_CORNER)
        .translate((CAB_CX, 0, 0)))
try:
    body = body.faces("<Y").edges().chamfer(FRONT_CH)
except Exception as e:
    print("  (chamfrein facade ignore : %s)" % e)

cavity = (cq.Workplane("XY")
          .box(CAB_W - 2 * WALL, DEPTH + 10, CAB_H - 2 * WALL, centered=(True, False, False))
          .edges("|Y").fillet(R_CORNER - WALL / 2)
          .translate((CAB_CX, FRONT_T, WALL)))
body = body.cut(cavity)

# Fenetre ecran : trou traversant + chanfrein en pente (repris du boitier radio)
body = body.cut(cq.Workplane("XY").box(WIN_OPEN_W, FRONT_T + 4, WIN_H, centered=(True, True, True))
                .translate((WIN_CX, FRONT_T / 2.0, Z_BOARD)))
cham = (cq.Workplane("XY").rect(WIN_OPEN_W + 2 * WIN_CH, WIN_H + 2 * WIN_CH)
        .workplane(offset=WIN_CH).rect(WIN_OPEN_W, WIN_H).loft())
cham = cham.rotate((0, 0, 0), (1, 0, 0), -90).translate((WIN_CX, 0, Z_BOARD))
body = body.cut(cham)

# Tablette sous le HP + nervures gauche/droite/haut, encoche fils a mi-hauteur droite
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

# Feuillure de la plaque grille + ouverture son elliptique traversante
body = body.cut(plate)
body = body.cut(yell(SND_AX, SND_AZ, -1, FRONT_T + 2, CAB_CX, HP_Z))

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

# Trou micro dans la facade (le son atteint le micro par le jeu facade/carte)
body = body.cut(ycyl(MIC_R, -1, FRONT_T + 1, MIC_X, MIC_Z))

# Bossages de vissage du capot (courts, au bord arriere, derriere le PCB)
BOSS_BACK = DEPTH - LID_T
for (x, z) in CAP_PTS:
    body = body.union(ycyl(CAP_BOSS_R, BOSS_BACK - CAP_BOSS_LEN, BOSS_BACK, x, z))
    body = body.cut(ycyl(CAP_PILOT, BOSS_BACK - CAP_BOSS_LEN, BOSS_BACK + 0.1, x, z))

# ============================================================================
# 2. Grille : la plaque, percee (borgne partout, traversant au droit du HP)
# ============================================================================
grille = plate
if PTS_BLIND:
    grille = grille.cut(yholes(PTS_BLIND, HOLE_D / 2, -0.5, BLIND_DEPTH))
if PTS_THRU:
    grille = grille.cut(yholes(PTS_THRU, HOLE_D / 2, -0.5, PLATE_T + 0.5))

# ============================================================================
# 3. Capot arriere plat (4 vis, segments de levre au milieu des murs)
# ============================================================================
capot = (cq.Workplane("XY")
         .box(CAB_W - 2 * LID_GAP, LID_T, CAB_H - 2 * LID_GAP, centered=(True, False, False))
         .edges("|Y").fillet(R_CORNER - LID_GAP)
         .translate((CAB_CX, BOSS_BACK, LID_GAP)))
CAV_XL, CAV_XR = X_LEFT + WALL, X_RIGHT - WALL
CAV_ZB, CAV_ZT = WALL, CAB_H - WALL
lips = [
    (LIP_LEN, LIP_T, CAB_CX, CAV_ZB + LIP_CLR),                   # bas
    (LIP_LEN, LIP_T, CAB_CX, CAV_ZT - LIP_CLR - LIP_T),           # haut
    (LIP_T, LIP_LEN, CAV_XL + LIP_CLR + LIP_T / 2, CAB_H / 2 - LIP_LEN / 2),   # gauche
    (LIP_T, LIP_LEN, CAV_XR - LIP_CLR - LIP_T / 2, CAB_H / 2 - LIP_LEN / 2),   # droit
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
assert PLATE_Z0 > Z_BOARD + WIN_H / 2 + WIN_CH, "plaque vs chanfrein ecran"
assert HP_Z + SND_AZ < z_top, "ouverture son vs nervure haute"
assert HP_STOP_W / 2 + 1 < xr, "plot capot vs nervures laterales"
assert HP_Z + HP_STOP_H / 2 < z_top, "plot capot vs nervure haute"
assert SHELF_Z + SHELF_T < HP_Z - HP_STOP_H / 2, "plot capot vs tablette"
assert MIC_X + MIC_R < WIN_CX - WIN_OPEN_W / 2 - WIN_CH, "trou micro vs chanfrein fenetre"

# ============================================================================
# Export
# ============================================================================
parts = {
    "es3c28p_seventies_corps.stl": body,
    "es3c28p_seventies_capot.stl": capot,
    "es3c28p_seventies_grille.stl": grille,
}
for name, part in parts.items():
    cq.exporters.export(part, "./" + name)
    bb = part.val().BoundingBox()
    print("%-26s  X[%6.1f %6.1f]  Y[%6.1f %6.1f]  Z[%6.1f %6.1f]" % (
        name, bb.xmin, bb.xmax, bb.ymin, bb.ymax, bb.zmin, bb.zmax))

# STEP combine (corps + grille) : memes positions monde + couleurs. Importe dans
# Bambu/Prusa, la grille reste dans sa feuillure -> multi-couleur.
asm = cq.Assembly()
asm.add(body, name="corps", color=cq.Color(0.93, 0.91, 0.86))
asm.add(grille, name="grille", color=cq.Color(0.17, 0.17, 0.17))
asm.export("es3c28p_seventies_corps+grille.step")
print("es3c28p_seventies_corps+grille.step  (corps+grille positionnes+colores)")
print("boite %.1f x %.1f x %.1f, trous grille : %d borgnes + %d traversants"
      % (CAB_W, DEPTH, CAB_H, len(PTS_BLIND), len(PTS_THRU)))
print("rappel slicing : compensation elephant foot ON (rives des trous couche 1)")

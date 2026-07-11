"""
Cabinet "vieille radio" pour la carte LCDWIKI ES3C28P.

Forme : coin a facade VERTICALE et DOS EN PENTE (capot arriere visse).
Imprime FACE CONTRE LE PLATEAU (facade en bas) : entretoises/bossages verticaux.

Repere : X = largeur, Y = profondeur (facade Y=0, arriere +Y), Z = hauteur (sol Z=0).
Lancer : python3 es3c28p-radio-stl.py
"""
import math
import cadquery as cq

# ---- Cotes carte ----
PCB_W, PCB_H, PCB_T = 86.0, 50.0, 1.6
HOLE_DX, HOLE_DZ = 39.0, 21.0
WIN_W, WIN_H = 60.5, 46.5
SPK_W, SPK_H, SPK_T = 40.44, 28.16, 9.15

# ---- Cabinet (largeur asymetrique : ecran centre sur la facade, USB pres du bord droit) ----
FRONT_T = 2.5
WALL = 2.0
LID_T = 3.0
X_LEFT = -50.0                 # facade elargie a gauche (recentre l'ecran decale)
X_RIGHT = 45.0                 # petit cote droit rapproche (USB plus pres du bord)
CAB_W = X_RIGHT - X_LEFT       # 94
CAB_CX = (X_LEFT + X_RIGHT) / 2.0   # -2 : centre de la facade
CAB_H = 96.0
DEPTH_BASE = 50.0
DEPTH_TOP = 12.0
R_TOP = 8.0
R_SIDE = 4.0

# ---- Ecran : ouverture agrandie 4 mm a gauche, centree sur la facade, + pente (chanfrein) ----
WIN_OPEN_W = WIN_W + 3.0       # +4 mm gauche / -1 mm droite vs origine (ecran decale)
WIN_CX = CAB_CX                # ecran centre sur la facade
WIN_CH = 3.0                   # pente entre facade et ecran (adoucit le toucher)
Z_BOARD = 33.0
PCB_FRONT = FRONT_T + 4.3
PCB_BACK = PCB_FRONT + PCB_T
STANDOFF_R = 3.5
BOARD_PILOT = 1.25

HP_Z = 76.0
OVAL_AX, OVAL_AZ = 21.5, 15.0  # ovale de la grille : contient TOUS les trous (aucun coupe)
SND_AX, SND_AZ = 20.5, 13.0    # ouverture son : contient tous les trous, bas au-dessus du separateur
GRILLE_T = 1.0                 # grille 2x plus fine
HOLE_D, HOLE_PITCH = 2.6, 4.2

USB_Y = 10.0
USB_OPEN_W, USB_OPEN_H, USB_OPEN_R = 10.5, 6.0, 1.0

# vis du capot : 4 coins ; hautes descendues (sinon percent la facade) ; +1 mm en Z
CAP_PTS = [(-45.5, 9.0), (40.5, 9.0), (-45.5, 75.0), (40.5, 75.0)]  # rentres : ne debordent plus des murs
CAP_BOSS_R = 3.5
CAP_BOSS_LEN = 11.0
CAP_PILOT = 1.35
CAP_SCREW_CLR = 1.8
CAP_HEAD_R = 3.3
CAP_HEAD_D = 1.6

# ---- geometrie du dos en pente ----
_t = (-(DEPTH_BASE - DEPTH_TOP), CAB_H)
SLANT = math.hypot(*_t)
NY, NZ = _t[1] / SLANT, -_t[0] / SLANT
C_IN = NY * DEPTH_BASE - LID_T


def depth_at(z):
    return DEPTH_BASE + (DEPTH_TOP - DEPTH_BASE) * (z / CAB_H)


def y_rebate(z):
    return (C_IN - NZ * z) / NY


def wedge(prof, width):
    return (cq.Workplane("YZ").polyline(prof).close()
            .extrude(width).translate((-width / 2.0, 0, 0)))


def ycyl(r, y0, y1, x, z):
    c = cq.Workplane("XY").circle(r).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90)
    return c.translate((x, y0, z))


def yell(ax, az, y0, y1, cx, cz):
    """Plaque/creux elliptique (ax en X, az en Z), le long de Y de y0 a y1."""
    e = cq.Workplane("XY").ellipse(ax, az).extrude(y1 - y0).rotate((0, 0, 0), (1, 0, 0), -90)
    return e.translate((cx, y0, cz))


def sf(solid, sel, r):
    try:
        return solid.edges(sel).fillet(r)
    except Exception as e:
        print("  (fillet ignore %s r=%s : %s)" % (sel, r, e))
        return solid


# ============================================================================
# Coque (facade verticale + dos en pente), decalee a CAB_CX (asymetrie)
# ============================================================================
outer = wedge([(0, 0), (DEPTH_BASE, 0), (DEPTH_TOP, CAB_H), (0, CAB_H)], CAB_W)
outer = sf(outer, "|Y and >Z", R_TOP)
outer = sf(outer, "|Y and <Z", R_SIDE)
outer = outer.translate((CAB_CX, 0, 0))

lid_quad = [(DEPTH_BASE, 0), (DEPTH_TOP, CAB_H),
            (DEPTH_TOP - LID_T * NY, CAB_H - LID_T * NZ),
            (DEPTH_BASE - LID_T * NY, -LID_T * NZ)]
lid_slab = wedge(lid_quad, CAB_W + 2).translate((CAB_CX, 0, 0))

# ============================================================================
# 1. Corps : facade + base + cotes + dessus, ouvert a l'arriere
# ============================================================================
cavity = wedge([(FRONT_T, WALL), (DEPTH_BASE + 15, WALL),
                (DEPTH_TOP + 15, CAB_H - WALL), (FRONT_T, CAB_H - WALL)],
               CAB_W - 2 * WALL)
# arrondir les coins hauts de la cavite (sinon ils percent les epaules arrondies)
cavity = sf(cavity, "|Y and >Z", R_TOP)
cavity = cavity.translate((CAB_CX, 0, 0))
body = outer.cut(cavity)

# Fenetre ecran : trou traversant (centre sur la facade, agrandi a gauche) + chanfrein
body = body.cut(cq.Workplane("XY").box(WIN_OPEN_W, FRONT_T + 4, WIN_H, centered=(True, True, True))
                .translate((WIN_CX, FRONT_T / 2.0, Z_BOARD)))
cham = (cq.Workplane("XY").rect(WIN_OPEN_W + 2 * WIN_CH, WIN_H + 2 * WIN_CH)
        .workplane(offset=WIN_CH).rect(WIN_OPEN_W, WIN_H).loft())
cham = cham.rotate((0, 0, 0), (1, 0, 0), -90).translate((WIN_CX, 0, Z_BOARD))
body = body.cut(cham)

# Separateur horizontal ecran / HP : le HP repose legerement dessus (renfort)
SHELF_Z, SHELF_T, SHELF_DEPTH = 60.0, 2.0, 13.0
body = body.union(cq.Workplane("XY").box(CAB_W - 2 * WALL + 1, SHELF_DEPTH, SHELF_T, centered=(True, False, False))
                  .translate((CAB_CX, FRONT_T - 1, SHELF_Z)))

# Cales de maintien du HP (41 x 28.5 x 10) : nervures gauche/droite/haut ; il
# repose sur le separateur (bas) ; encoche fils au milieu du cote droit.
SPK2_W, SPK2_H = 41.0, 28.5
SPK_CLR, RIBT, RIBD = 0.5, 2.0, 7.0
z_bot = SHELF_Z + SHELF_T                 # 62 : dessus du separateur
xr = SPK2_W / 2.0 + SPK_CLR               # bord HP + jeu
z_top = z_bot + SPK2_H + SPK_CLR
z_mid = z_bot + SPK2_H / 2.0              # milieu = arrivee des fils


def rib(w, h, x, z):
    return (cq.Workplane("XY").box(w, RIBD, h, centered=(True, False, False))
            .translate((x, FRONT_T - 1, z)))


body = body.union(rib(RIBT, SPK2_H + 2 * SPK_CLR, CAB_CX - xr - RIBT / 2, z_bot - SPK_CLR))   # gauche
rr = rib(RIBT, SPK2_H + 2 * SPK_CLR, CAB_CX + xr + RIBT / 2, z_bot - SPK_CLR)                  # droit
rr = rr.cut(cq.Workplane("XY").box(RIBT + 2, RIBD + 4, 7.0, centered=(True, True, True))
            .translate((CAB_CX + xr + RIBT / 2, FRONT_T - 1 + RIBD / 2, z_mid)))               # encoche fils
body = body.union(rr)
body = body.union(rib(2 * xr + 2 * RIBT, RIBT, CAB_CX, z_top))                                 # haut

# Grille : feuillure OVALE (accueille la grille a fleur) + ouverture son ovale traversante
body = body.cut(yell(OVAL_AX, OVAL_AZ, 0, GRILLE_T, CAB_CX, HP_Z))
body = body.cut(yell(SND_AX, SND_AZ, -1, FRONT_T + 2, CAB_CX, HP_Z))

# Entretoises de montage carte (positions inchangees : bord de la carte, X=0)
BOARD_PTS = [(-HOLE_DX, Z_BOARD - HOLE_DZ), (HOLE_DX, Z_BOARD - HOLE_DZ),
             (-HOLE_DX, Z_BOARD + HOLE_DZ), (HOLE_DX, Z_BOARD + HOLE_DZ)]
for (x, z) in BOARD_PTS:
    body = body.union(ycyl(STANDOFF_R, FRONT_T - 1, PCB_FRONT, x, z))
    body = body.cut(ycyl(BOARD_PILOT, FRONT_T + 1, PCB_FRONT + 0.5, x, z))

# Decoupe USB sur le flanc droit
usb = cq.Workplane("XY").box(2 * WALL + 6, USB_OPEN_H, USB_OPEN_W, centered=(True, True, True))
usb = sf(usb, "|X", USB_OPEN_R)
body = body.cut(usb.translate((X_RIGHT, USB_Y, Z_BOARD)))

# Bossages de vissage du capot (courts, pres du rim, accoles aux murs)
for (x, z) in CAP_PTS:
    ry = y_rebate(z)
    body = body.union(ycyl(CAP_BOSS_R, ry - CAP_BOSS_LEN, ry, x, z))
    body = body.cut(ycyl(CAP_PILOT, ry - CAP_BOSS_LEN, ry + 0.1, x, z))

# ============================================================================
# 2. Capot arriere (dos en pente, visse)
# ============================================================================
capot = lid_slab.intersect(outer)
capot = capot.intersect(cq.Workplane("XY").box(CAB_W - 0.6, 400, 400, centered=True).translate((CAB_CX, 0, 0)))
for (x, z) in CAP_PTS:
    ry, oy = y_rebate(z), depth_at(z)
    capot = capot.cut(ycyl(CAP_SCREW_CLR, ry - 1, oy + 1, x, z))
    capot = capot.cut(ycyl(CAP_HEAD_R, oy - CAP_HEAD_D, oy + 1, x, z))

# ============================================================================
# 3. Grille HP (plaque fine, trous ronds traversants)
# ============================================================================
# plaque OVALE (remplit exactement la feuillure -> se soude au corps en multi-couleur)
grille = yell(OVAL_AX, OVAL_AZ, 0, GRILLE_T, CAB_CX, HP_Z)
ax, az = (SPK_W - 4) / 2.0, (SPK_H - 4) / 2.0
nx, nz = int(ax // HOLE_PITCH), int(az // HOLE_PITCH)
for i in range(-nx, nx + 1):
    for j in range(-nz, nz + 1):
        hx, hz = i * HOLE_PITCH, j * HOLE_PITCH
        if (hx / ax) ** 2 + (hz / az) ** 2 <= 1.0:
            grille = grille.cut(ycyl(HOLE_D / 2.0, -1, GRILLE_T + 1, CAB_CX + hx, HP_Z + hz))

# ============================================================================
# Export
# ============================================================================
parts = {
    "es3c28p_radio_corps.stl": body,
    "es3c28p_radio_capot.stl": capot,
    "es3c28p_radio_grille.stl": grille,
}
for name, part in parts.items():
    cq.exporters.export(part, "./" + name)
    bb = part.val().BoundingBox()
    print("%-26s  X[%6.1f %6.1f]  Y[%6.1f %6.1f]  Z[%6.1f %6.1f]" % (
        name, bb.xmin, bb.xmax, bb.ymin, bb.ymax, bb.zmin, bb.zmax))

# STEP combine (corps + grille) : memes positions monde + couleurs. Importe dans
# Bambu/Prusa, la grille reste dans sa feuillure (positions conservees) -> multi-couleur.
asm = cq.Assembly()
asm.add(body, name="corps", color=cq.Color(0.79, 0.72, 0.55))
asm.add(grille, name="grille", color=cq.Color(0.48, 0.29, 0.16))
asm.export("es3c28p_radio_corps+grille.step")
print("es3c28p_radio_corps+grille.step  (corps+grille positionnes+colores)")
print("largeur %.0f (gauche %.0f / droite %.0f), hauteur %.0f, ecran centre X=%.1f"
      % (CAB_W, X_LEFT, X_RIGHT, CAB_H, CAB_CX))

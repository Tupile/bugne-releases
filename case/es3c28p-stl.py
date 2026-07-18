"""
Maquette ES3C28P (LCDWIKI ESP32-S3 2.8" tactile) + boitier 2 pieces,
speaker au dos de la carte, son vers l'arriere par grille dans le fond.
Cotes carte issues du plan mecanique du datasheet (page 11).
Origine XY = centre du PCB. Z=0 = dessous du boitier pour le fond.
"""
import cadquery as cq

# ---- Cotes carte (datasheet) ----
PCB_W, PCB_H, PCB_T = 50.0, 86.0, 1.6
PCB_R = 3.5
HOLE_D = 3.2
HOLE_X, HOLE_Y = 21.0, 39.0
STACK_T = 4.3
GLASS_W, GLASS_H = 50.0, 69.2
GLASS_Y_OFF = 0.2
SMD_T = 4.7
USB_X = -0.1                     # mesure : centre a 25.1 du bord droit (carte 50)
USB_W, USB_T = 9.0, 3.2
USB_CUT_W, USB_CUT_H = 13.0, 8.0

# ---- Speaker fourni (mesure) ----
SPK_W, SPK_H, SPK_T = 40.44, 28.16, 9.15  # paysage, membrane vers l'arriere
SPK_CLR = 0.5                    # jeu dans le cadre
SPK_GAP = 0.5                    # jeu entre speaker et composants
SPK_YC = 18.0                    # centre du speaker, proche du port SPEAKER
RIB_T, RIB_H = 1.6, 5.0          # cadre de maintien

# ---- Parametres boitier ----
CLR = 1.0
WALL = 2.0
FLOOR = 2.0
STANDOFF_H = SPK_T + SPK_GAP + SMD_T     # 14.35 : speaker derriere les SMD
STANDOFF_D = 7.0
PILOT_D = 2.7
WIN_W, WIN_H = 46.5, 60.5
WIN_OPEN_H = WIN_H + 3.0         # ouverture allongee (ecran decale sur la carte)
WIN_Y = 2.25                     # centre ecran reel (vers le haut), repris du seventies
WIN_CH = 3.0                     # chanfrein en pente autour de l'ecran
COVER_T = 2.0
SCREW_D = 3.4

IN_W, IN_H = PCB_W + 2 * CLR, PCB_H + 2 * CLR
OUT_W, OUT_H = IN_W + 2 * WALL, IN_H + 2 * WALL
CAVITY_DEPTH = STANDOFF_H + PCB_T + STACK_T
BOX_Z = FLOOR + CAVITY_DEPTH

# =================== 1. Maquette de la carte ===================
pcb = (cq.Workplane("XY")
       .box(PCB_W, PCB_H, PCB_T, centered=(True, True, False))
       .edges("|Z").fillet(PCB_R))
for x in (-HOLE_X, HOLE_X):
    for y in (-HOLE_Y, HOLE_Y):
        pcb = pcb.cut(cq.Workplane("XY").moveTo(x, y)
                      .circle(HOLE_D / 2).extrude(PCB_T))
stack = (cq.Workplane("XY", origin=(0, GLASS_Y_OFF, PCB_T))
         .box(GLASS_W, GLASS_H, STACK_T, centered=(True, True, False)))
smd = (cq.Workplane("XY", origin=(0, 0, -SMD_T))
       .box(36, 68, SMD_T, centered=(True, True, False)))
usb = (cq.Workplane("XY", origin=(USB_X, -PCB_H / 2 - 1.5 + 8.5 / 2, -USB_T))
       .box(USB_W, 8.5, USB_T, centered=(True, True, False)))
board = pcb.union(stack).union(smd).union(usb)

# =================== 2. Fond du boitier ===================
shell = (cq.Workplane("XY")
         .box(OUT_W, OUT_H, BOX_Z, centered=(True, True, False))
         .edges("|Z").fillet(PCB_R + WALL / 2)
         .cut(cq.Workplane("XY", origin=(0, 0, FLOOR))
              .box(IN_W, IN_H, CAVITY_DEPTH + 1, centered=(True, True, False))
              .edges("|Z").fillet(PCB_R)))

# Cadre de maintien du speaker (anneau rectangulaire sur le plancher)
fw, fh = SPK_W + 2 * SPK_CLR, SPK_H + 2 * SPK_CLR
frame = (cq.Workplane("XY", origin=(0, SPK_YC, FLOOR))
         .box(fw + 2 * RIB_T, fh + 2 * RIB_T, RIB_H, centered=(True, True, False))
         .edges("|Z").fillet(2.0)
         .cut(cq.Workplane("XY", origin=(0, SPK_YC, FLOOR))
              .box(fw, fh, RIB_H + 1, centered=(True, True, False))
              .edges("|Z").fillet(1.5)))
shell = shell.union(frame)

# Encoches passage cable, centrees sur les deux petits cotes du cadre
for sx in (-1, 1):
    shell = shell.cut(cq.Workplane("XY",
                      origin=(sx * (fw + RIB_T) / 2, SPK_YC, FLOOR))
                      .box(RIB_T + 2, 7, RIB_H + 1, centered=(True, True, False)))

# Grille dans le plancher : trame de trous 2.2 limitee a une ellipse
ga, gb = (SPK_W - 10) / 2, (SPK_H - 10) / 2
step = 4.5
grid = [(i * step, j * step)
        for i in range(-int(ga // step), int(ga // step) + 1)
        for j in range(-int(gb // step), int(gb // step) + 1)
        if (i * step / ga) ** 2 + (j * step / gb) ** 2 <= 1.0]
for gx, gy in grid:
    shell = shell.cut(cq.Workplane("XY", origin=(gx, SPK_YC + gy, 0))
                      .circle(1.1).extrude(FLOOR))

# Entretoises carte
for x in (-HOLE_X, HOLE_X):
    for y in (-HOLE_Y, HOLE_Y):
        post = (cq.Workplane("XY", origin=(x, y, FLOOR))
                .circle(STANDOFF_D / 2).extrude(STANDOFF_H)
                .cut(cq.Workplane("XY", origin=(x, y, FLOOR))
                     .circle(PILOT_D / 2).extrude(STANDOFF_H)))
        shell = shell.union(post)

# Decoupe passage cable USB-C, paroi basse
usb_z = FLOOR + STANDOFF_H - USB_T
shell = shell.cut(cq.Workplane("XY",
                  origin=(USB_X - USB_CUT_W / 2, -OUT_H / 2 - 1, usb_z - 2.4))
                  .box(USB_CUT_W, WALL + 5, USB_CUT_H,
                       centered=(False, False, False)))

# =================== 3. Facade ===================
cover = (cq.Workplane("XY")
         .box(OUT_W, OUT_H, COVER_T, centered=(True, True, False))
         .edges("|Z").fillet(PCB_R + WALL / 2)
         .cut(cq.Workplane("XY", origin=(0, WIN_Y, 0))
              .box(WIN_W, WIN_OPEN_H, COVER_T, centered=(True, True, False))))
# Chanfrein en pente autour de l'ecran (repris du seventies), depuis la face
# exterieure ; le petit rectangle finit derriere la face interne (COVER_T <
# WIN_CH) : tout le bord de fenetre est en pente.
cham = (cq.Workplane("XY", origin=(0, WIN_Y, COVER_T))
        .rect(WIN_W + 2 * WIN_CH, WIN_OPEN_H + 2 * WIN_CH)
        .workplane(offset=-WIN_CH)
        .rect(WIN_W, WIN_OPEN_H).loft())
cover = cover.cut(cham)
for x in (-HOLE_X, HOLE_X):
    for y in (-HOLE_Y, HOLE_Y):
        cover = cover.union(cq.Workplane("XY", origin=(x, y, -STACK_T))
                            .circle(STANDOFF_D / 2).extrude(STACK_T))
        cover = cover.cut(cq.Workplane("XY", origin=(x, y, -STACK_T))
                          .circle(SCREW_D / 2).extrude(STACK_T + COVER_T))
# Trou micro, position approximative (a verifier sur la carte) ; decale de
# 2.5 vers le haut du micro pour eviter le chanfrein fenetre (le son atteint
# le micro par le jeu facade/carte), comme le seventies
MIC_X, MIC_Y, MIC_R = 15, 38.5, 1.0
cover = cover.cut(cq.Workplane("XY", origin=(MIC_X, MIC_Y, -STACK_T))
                  .circle(MIC_R).extrude(STACK_T + COVER_T))

# =================== Controles geometriques ===================
CHAM_Y_TOP = WIN_Y + WIN_OPEN_H / 2 + WIN_CH        # emprise haute du chanfrein
assert MIC_Y - MIC_R > CHAM_Y_TOP, "trou micro vs chanfrein fenetre"
assert HOLE_Y - SCREW_D / 2 > CHAM_Y_TOP, "trous de vis vs chanfrein fenetre"
assert WIN_Y + WIN_OPEN_H / 2 < GLASS_Y_OFF + GLASS_H / 2, "ouverture vs dalle"

# =================== Export ===================
cq.exporters.export(board, "./es3c28p_carte.stl")
cq.exporters.export(shell, "./es3c28p_boitier_fond.stl")
cq.exporters.export(cover, "./es3c28p_boitier_facade.stl")
print("OK  exterieur =", OUT_W, "x", OUT_H, "x", round(BOX_Z + COVER_T, 1), "mm")

#!/bin/sh
# Visible-VM walkthrough of Files, the desktop icons, associations and the trash, with real pointer events
# (--abs: the same absolute pointer as `make run`). Screenshots go to build/filesshots/; each scenario boots its own VM (disk state carries over between them; the run starts from a fresh rootfs).
# Coordinates assume the default 1280x800 mode and the smart window placement (Files lands at 328,89).
# Run after `make`:  sh tools/filestest/vm.sh
cd "$(dirname "$0")/../.."
rm -f build/root.ext2 build/vibeos.img && make -j16 >/dev/null || exit 1    # vmtest boots the persistent image: start every run from a fresh rootfs
O=build/filesshots
mkdir -p $O
vt() { python3 tools/vmtest.py --abs --timeout 60 "wait:[desktop] ready" sleep:2 "$@" > build/filesshots/last.log 2>&1 || { echo "FAILED: $*"; tail -20 build/filesshots/last.log; exit 1; }; }
FILES='type:files /home/Pictures &\n'

# desktop: icons, drag to a new cell, double-click opens through open.conf, icon menu
vt drag:455,33,1100,700 sleep:1 shot:$O/01-desktop.png drag:62,260,500,300 sleep:1 moveto:500,300 rclick sleep:1 shot:$O/02-desktop-menu.png moveto:800,500 click sleep:0.5 moveto:62,155 dclick sleep:3 shot:$O/03-desktop-open-image.png

# multi-select (Ctrl+click), rubber band
vt "$FILES" sleep:4 moveto:558,225 click sleep:0.3 keydown:ctrl moveto:870,225 click moveto:1182,225 click keyup:ctrl sleep:0.5 shot:$O/10-ctrl-select.png moveto:640,500 down moveto:900,320 sleep:0.3 shot:$O/11-rubber-band.png up

# list view, sort by clicking a header, search box, context menu, properties
vt "$FILES" sleep:4 moveto:1191,149 click sleep:0.5 moveto:965,190 click sleep:0.5 shot:$O/12-list-sorted.png moveto:1050,149 click type:dunes sleep:0.5 moveto:700,260 rclick sleep:0.5 shot:$O/13-search-context-menu.png moveto:700,262 click sleep:0.3 key:esc moveto:640,500 click key:esc sleep:0.3 moveto:570,218 click key:ctrl-i sleep:0.8 shot:$O/14-properties.png

# trash: Delete key, the Trash view, restore from the context menu
vt "type:files /home/Documents &\n" sleep:4 moveto:1078,225 click key:delete sleep:0.8 shot:$O/20-moved-to-trash.png moveto:390,378 click sleep:0.8 moveto:558,225 rclick sleep:0.5 shot:$O/21-trash-menu.png moveto:600,315 click sleep:0.8 shot:$O/22-restored.png

# drag and drop: onto a folder inside Files, onto the desktop wallpaper, from the desktop back into Files
vt "type:files /home/Documents &\n" sleep:4 drag:455,33,1100,700 sleep:0.5 drag:974,225,558,225 sleep:1.5 drag:662,225,200,520 sleep:2 shot:$O/30-dnd-to-desktop.png drag:62,255,700,400 sleep:2 shot:$O/31-dnd-from-desktop.png

# keyboard: arrows, Enter opens an image in the viewer; rename (F2), new folder, copy and paste
vt "$FILES" sleep:4 moveto:558,225 click sleep:0.3 key:right key:right key:ret sleep:3 shot:$O/40-open-image.png
vt "type:files /home/Documents &\n" sleep:4 moveto:662,225 click key:ctrl-c key:ctrl-v sleep:1 moveto:870,225 click key:f2 sleep:0.6 shot:$O/41-rename.png key:ctrl-u "type:renamed.html\n" sleep:1 moveto:1230,149 click sleep:0.6 "type:Stuff\n" sleep:1 shot:$O/42-copy-rename-newfolder.png

# the in-VM logic test (open -w, trash)
vt "type:sh /home/filestest.sh\n" sleep:8 shot:$O/50-filestest.png
echo "screenshots in $O"

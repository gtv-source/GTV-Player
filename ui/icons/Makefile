
# Regenerate bitmaps from the SVG using inkscape CLI export
# and ImageMagick. Don't use ImageMagick for the initial
# SVG conversion, since it merely calls inkscape, but uses
# 96 DPI res resulting in poor quality output.

regenerate:
	for s in 32 128; \
	do \
	    inkscape --without-gui --export-png=gtv_$${s}x$${s}.png ; \
	done
// THE WIZARD'S APP ICON (Kathy, 13 Oct): macOS style - a rounded square in EchoJay's ink, the EchoJay mark (the "o" with its arc,
// cropped from the welcome screen's logo, Source/EchoJayLogo.h) in white, centred; the beta adds a "BETA" ribbon in the top-right corner.
// Every size of the iconset is RENDERED at its own pixel size (no downscaling), then `iconutil -c icns` builds the .icns.
//   swift make_icon.swift <logo.png> <out dir> [--beta | --test]   (14 Oct: --test, an orange "TEST" ribbon, for EchoJay Wizard Test)   ->  <out dir>/<name>.iconset/*.png, <out dir>/<name>.icns, <out dir>/<name>_1024.png
// The wordmark is too wide to read at 16-32 px, so the icon carries the mark alone.
import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count >= 3 else { print("usage: make_icon.swift <logo.png> <out dir> [--beta | --test]"); exit(2) }
let test = args.contains("--test")
let beta = args.contains("--beta") || test   // the test build wears the same ribbon, in its own colour and word
let ribbonWord = test ? "TEST" : "BETA"
let outDir = URL(fileURLWithPath: args[2], isDirectory: true)
let name = test ? "WizardTest" : beta ? "WizardBeta" : "Wizard"

guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil), let logo = CGImageSourceCreateImageAtIndex(src, 0, nil)
else { print("cannot read the logo"); exit(2) }

// THE MARK: the logo's fourth glyph ("o" with its arc), found by its alpha columns - not a hard-coded rectangle
func alphaBuffer (_ img: CGImage) -> [UInt8] {
    var buf = [UInt8](repeating: 0, count: img.width * img.height * 4)
    let ctx = CGContext(data: &buf, width: img.width, height: img.height, bitsPerComponent: 8, bytesPerRow: img.width * 4,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
    return buf
}
let w = logo.width, h = logo.height, buf = alphaBuffer(logo)
var runs: [(Int, Int)] = []; var start = -1
for x in 0..<w {
    var any = false; for y in 0..<h where buf[(y * w + x) * 4 + 3] > 40 { any = true; break }
    if any && start < 0 { start = x }; if !any && start >= 0 { runs.append((start, x - 1)); start = -1 }
}
if start >= 0 { runs.append((start, w - 1)) }
guard runs.count >= 4 else { print("expected the wordmark's glyphs, found \(runs.count) run(s)"); exit(3) }
let (ox0, ox1) = runs[3]
var oy0 = h, oy1 = 0
for y in 0..<h { for x in ox0...ox1 where buf[(y * w + x) * 4 + 3] > 40 { oy0 = min(oy0, y); oy1 = max(oy1, y); break } }
guard let mark = logo.cropping(to: CGRect(x: ox0, y: oy0, width: ox1 - ox0 + 1, height: oy1 - oy0 + 1)) else { print("crop failed"); exit(3) }
print("mark: columns \(ox0)-\(ox1), rows \(oy0)-\(oy1) of the \(w) x \(h) logo")

let ink = CGColor(red: 0x14 / 255.0, green: 0x18 / 255.0, blue: 0x24 / 255.0, alpha: 1)        // EchoJay's ink (the app's ink colour)
let accent = CGColor(red: 0x3d / 255.0, green: 0x5a / 255.0, blue: 0xfe / 255.0, alpha: 1)     // the app's accent (buttons)

// CLEAN AT ANY SIZE - THE MARK AS VECTORS: the only copies of the mark are ~192 px and hard-edged, so no resampling stays clean at
// 1024 px. Its geometry is MEASURED from the bitmap and drawn as paths: the outer circle (the glyph's bounds), the inner circle (fitted
// to the hole's edge points), and the cut - a band whose top and bottom edges are measured column by column where they cross the ring
// and fitted (least squares): a line on the left, a quadratic on the right (it widens and tilts up). Coordinates: the glyph's own
// pixels, y down; drawn flipped into the icon.
let G: (w: Double, h: Double, cx: Double, cy: Double, rOut: Double, rIn: Double, topL: [Double], botL: [Double], topR: [Double], botR: [Double]) = {
    let mw = mark.width, mh = mark.height
    var px = [UInt8](repeating: 0, count: mw * mh * 4)
    let c = CGContext(data: &px, width: mw, height: mh, bitsPerComponent: 8, bytesPerRow: mw * 4, space: CGColorSpaceCreateDeviceRGB(),
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.draw(mark, in: CGRect(x: 0, y: 0, width: mw, height: mh))
    func on (_ x: Int, _ y: Int) -> Bool { px[(y * mw + x) * 4 + 3] > 127 }
    let cx = Double (mw) / 2.0, cy = Double (mh) / 2.0, rOut = (Double (mw) + Double (mh)) / 4.0
    // the inner circle: along rows away from the cut, the hole's edges (first opaque pixel outwards from the centre)
    var ri: [Double] = []
    for y in 0..<mh where abs (Double (y) - cy) > 30 && abs (Double (y) - cy) < 40 {
        var xl = Int (cx); while xl > 0 && !on (xl, y) { xl -= 1 }; var xr = Int (cx); while xr < mw - 1 && !on (xr, y) { xr += 1 }
        if xl > 0 && xr < mw - 1 { for x in [Double (xl) + 0.5, Double (xr) - 0.5] { ri.append (((x - cx) * (x - cx) + (Double (y) - cy) * (Double (y) - cy)).squareRoot()) } }
    }
    let rIn = ri.isEmpty ? rOut * 0.5 : ri.reduce (0, +) / Double (ri.count)
    // the cut's edges per column, within the ring (outside the hole), in the band around the centre
    func edges (_ xs: [Int]) -> (top: [(Double, Double)], bot: [(Double, Double)]) {
        var top: [(Double, Double)] = [], bot: [(Double, Double)] = []
        for x in xs {
            let lo = Int (cy) - 40, hi = Int (cy) + 40; var y = lo
            while y < hi && on (x, y) { y += 1 }; let t = y            // first transparent row going down
            while y < hi && !on (x, y) { y += 1 }; let b = y          // first opaque row after the gap
            if t > lo && b < hi && b > t { top.append ((Double (x) + 0.5, Double (t))); bot.append ((Double (x) + 0.5, Double (b))) }
        }
        return (top, bot)
    }
    func fit (_ p: [(Double, Double)], _ deg: Int) -> [Double] {   // least squares y = a + b x (+ c x^2)
        let n = deg + 1; var A = [[Double]](repeating: [Double](repeating: 0, count: n + 1), count: n)
        for (x, y) in p { var pw = [Double](repeating: 1, count: 2 * n); for k in 1..<(2 * n) { pw[k] = pw[k - 1] * x }
            for r in 0..<n { for c in 0..<n { A[r][c] += pw[r + c] }; A[r][n] += pw[r] * y } }
        for c in 0..<n { let piv = (c..<n).max { abs (A[$0][c]) < abs (A[$1][c]) }!; A.swapAt (c, piv)
            for r in 0..<n where r != c { let f = A[r][c] / A[c][c]; for k in c...n { A[r][k] -= f * A[c][k] } } }
        return (0..<n).map { A[$0][n] / A[$0][$0] }
    }
    let inL = Int (cx - rIn) - 2, inR = Int (cx + rIn) + 2
    let L = edges (Array (2..<inL)), R = edges (Array (inR..<(mw - 2)))
    guard L.top.count > 5, R.top.count > 5 else { print ("the cut was not found in the mark"); exit (3) }
    return (Double (mw), Double (mh), cx, cy, rOut, rIn, fit (L.top, 1), fit (L.bot, 1), fit (R.top, 2), fit (R.bot, 2))
}()
func evalPoly (_ c: [Double], _ x: Double) -> Double { var y = 0.0, p = 1.0; for k in c { y += k * p; p *= x }; return y }
print (String (format: "mark as vectors: outer r %.1f, inner r %.1f, centre (%.1f, %.1f); the cut %.1f-%.1f on the left", G.rOut, G.rIn, G.cx, G.cy, evalPoly (G.topL, 10), evalPoly (G.botL, 10)))
func markAt (_ ow: Int, _ oh: Int) -> CGImage {
    let ow = max (ow, 1), oh = max (oh, 1)
    let c = CGContext(data: nil, width: ow, height: oh, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.setShouldAntialias (true); c.setAllowsAntialiasing (true)
    // a 4 % margin round the glyph, so the fitted circle (191 px across a 190 px glyph) is never clipped flat at the top and bottom
    let pad = 0.04, sx = Double (ow) / (G.w * (1 + 2 * pad)), sy = Double (oh) / (G.h * (1 + 2 * pad)), px0 = pad * G.w, py0 = pad * G.h
    func P (_ x: Double, _ y: Double) -> CGPoint { CGPoint (x: (x + px0) * sx, y: (G.h - y + py0) * sy) }   // glyph pixels (y down) -> the image (y up)
    // the ring
    let ring = CGMutablePath()
    let o = P (G.cx, G.cy)
    ring.addEllipse (in: CGRect (x: o.x - G.rOut * sx, y: o.y - G.rOut * sy, width: 2 * G.rOut * sx, height: 2 * G.rOut * sy))
    ring.addEllipse (in: CGRect (x: o.x - G.rIn * sx, y: o.y - G.rIn * sy, width: 2 * G.rIn * sx, height: 2 * G.rIn * sy))
    c.addPath (ring); c.setFillColor (CGColor (red: 1, green: 1, blue: 1, alpha: 1)); c.fillPath (using: .evenOdd)
    // the cut: top edge left -> right, bottom edge right -> left (left fits left of the centre, right fits right of it)
    let cut = CGMutablePath(); let x0 = -4.0, x1 = G.w + 4.0, steps = 96
    func top (_ x: Double) -> Double { x < G.cx ? evalPoly (G.topL, x) : evalPoly (G.topR, x) }
    func bot (_ x: Double) -> Double { x < G.cx ? evalPoly (G.botL, x) : evalPoly (G.botR, x) }
    cut.move (to: P (x0, top (x0))); for k in 1...steps { let x = x0 + (x1 - x0) * Double (k) / Double (steps); cut.addLine (to: P (x, top (x))) }
    for k in stride (from: steps, through: 0, by: -1) { let x = x0 + (x1 - x0) * Double (k) / Double (steps); cut.addLine (to: P (x, bot (x))) }
    cut.closeSubpath()
    c.setBlendMode (.clear); c.addPath (cut); c.fillPath(); c.setBlendMode (.normal)
    return c.makeImage()!
}

func render (_ px: Int) -> CGImage {
    let s = CGFloat(px)
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high; ctx.setShouldAntialias(true)
    // the macOS icon grid: an 824 / 1024 square, corner radius 185 / 1024, centred (Apple's Big Sur template)
    let inset = s * 100.0 / 1024.0, side = s * 824.0 / 1024.0, radius = s * 185.4 / 1024.0
    let tile = CGRect(x: inset, y: inset, width: side, height: side)
    let path = CGPath(roundedRect: tile, cornerWidth: radius, cornerHeight: radius, transform: nil)
    ctx.addPath(path); ctx.setFillColor(ink); ctx.fillPath()
    // the mark, white, 52 % of the tile's width, centred - through markAt (clean edges at any size)
    let mw = side * 0.52 * 1.08, mh = mw * CGFloat(mark.height) / CGFloat(mark.width)   // x 1.08: markAt's 4 % margin each side
    ctx.draw(markAt (Int (mw.rounded()), Int (mh.rounded())), in: CGRect(x: tile.midX - mw / 2, y: tile.midY - mh / 2, width: mw, height: mh))
    if beta {
        // the BETA ribbon: a diagonal band across the tile's top-right corner, clipped to the tile
        ctx.saveGState(); ctx.addPath(path); ctx.clip()
        let band = side * 0.17, c = CGPoint(x: tile.maxX - side * 0.20, y: tile.maxY - side * 0.20)
        ctx.translateBy(x: c.x, y: c.y); ctx.rotate(by: -.pi / 4)
        ctx.setFillColor(test ? CGColor(red: 0.96, green: 0.45, blue: 0.10, alpha: 1) : accent); ctx.fill(CGRect(x: -side, y: -band / 2, width: side * 2, height: band))
        if px >= 64 {   // the word only where it can be read; at 16 / 32 px the blue corner alone tells them apart
            let font = CTFontCreateWithName("Helvetica-Bold" as CFString, band * 0.62, nil)
            let attr = NSAttributedString(string: ribbonWord, attributes: [NSAttributedString.Key(kCTFontAttributeName as String): font,
                                                                      NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(red: 1, green: 1, blue: 1, alpha: 1)])
            let line = CTLineCreateWithAttributedString(attr), b = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
            ctx.textPosition = CGPoint(x: -b.width / 2 - b.minX, y: -b.height / 2 - b.minY); CTLineDraw(line, ctx)
        }
        ctx.restoreGState()
    }
    return ctx.makeImage()!
}
func writePNG (_ img: CGImage, _ url: URL) {
    let d = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, img, nil); CGImageDestinationFinalize(d)
}
let iconset = outDir.appendingPathComponent("\(name).iconset", isDirectory: true)
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for (base, scale) in [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)] {
    writePNG(render(base * scale), iconset.appendingPathComponent("icon_\(base)x\(base)\(scale == 2 ? "@2x" : "").png"))
}
writePNG(render(1024), outDir.appendingPathComponent("\(name)_1024.png"))
writePNG(render(32), outDir.appendingPathComponent("\(name)_32.png"))
let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
p.arguments = ["-c", "icns", iconset.path, "-o", outDir.appendingPathComponent("\(name).icns").path]
try p.run(); p.waitUntilExit()
print(p.terminationStatus == 0 ? "\(name).icns: 10 sizes, 16-1024 px" : "iconutil failed (\(p.terminationStatus))")
exit(p.terminationStatus)

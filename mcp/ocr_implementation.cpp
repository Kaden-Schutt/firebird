// OCR implementation for Nspire CX LCD
// Add to mcpserver.cpp

// ============ OCR Data Members (add to mcpserver.h private section) ============
// QHash<QByteArray, char> m_ocrGlyphTable;
// int m_ocrCharWidth = 0;
// int m_ocrCharHeight = 0;
// int m_ocrBaselineX = 0;  // pixel offset to first char column
// int m_ocrBaselineY = 0;  // pixel offset to first text line

// ============ Helper: framebuffer to binary bitmap ============

static QVector<bool> framebufferToBinary(const QImage &image, int threshold = 128)
{
    int w = image.width();
    int h = image.height();
    QVector<bool> binary(w * h, false);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            QRgb pixel = image.pixel(x, y);
            int gray = qGray(pixel);
            binary[y * w + x] = (gray < threshold); // true = dark (text)
        }
    }
    return binary;
}

// ============ Helper: detect font metrics from binary image ============

struct FontMetrics {
    int charWidth;
    int charHeight;
    int startX;    // first character column pixel
    int startY;    // first text line pixel
    int cols;      // chars per line
    int rows;      // text lines
    bool valid;
};

static FontMetrics detectFontMetrics(const QVector<bool> &binary, int imgW, int imgH)
{
    FontMetrics fm = {0, 0, 0, 0, 0, 0, false};

    // Count dark pixels per row to find text line boundaries
    QVector<int> rowDensity(imgH, 0);
    for (int y = 0; y < imgH; ++y) {
        for (int x = 0; x < imgW; ++x) {
            if (binary[y * imgW + x]) rowDensity[y]++;
        }
    }

    // Find text lines: groups of consecutive rows with dark pixels
    QVector<QPair<int,int>> textBands; // (startY, endY)
    bool inBand = false;
    int bandStart = 0;
    int minDensity = imgW / 100; // at least 1% of width has dark pixels

    for (int y = 0; y < imgH; ++y) {
        if (rowDensity[y] > minDensity) {
            if (!inBand) { bandStart = y; inBand = true; }
        } else {
            if (inBand) {
                textBands.append({bandStart, y});
                inBand = false;
            }
        }
    }
    if (inBand) textBands.append({bandStart, imgH});

    if (textBands.size() < 2) {
        // Try with lower threshold
        minDensity = 1;
        textBands.clear();
        inBand = false;
        for (int y = 0; y < imgH; ++y) {
            if (rowDensity[y] > minDensity) {
                if (!inBand) { bandStart = y; inBand = true; }
            } else {
                if (inBand) { textBands.append({bandStart, y}); inBand = false; }
            }
        }
        if (inBand) textBands.append({bandStart, imgH});
    }

    if (textBands.size() < 2) return fm;

    // Character height = most common band height
    QMap<int, int> heightCounts;
    for (const auto &band : textBands) {
        int h = band.second - band.first;
        if (h >= 6 && h <= 20) heightCounts[h]++;
    }

    int bestHeight = 0, bestCount = 0;
    for (auto it = heightCounts.begin(); it != heightCounts.end(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            bestHeight = it.key();
        }
    }

    if (bestHeight == 0) return fm;

    // Line spacing = distance between starts of consecutive bands
    QVector<int> spacings;
    for (int i = 1; i < textBands.size(); ++i) {
        int spacing = textBands[i].first - textBands[i-1].first;
        if (spacing >= bestHeight && spacing <= bestHeight * 2)
            spacings.append(spacing);
    }

    int lineHeight = bestHeight;
    if (!spacings.isEmpty()) {
        std::sort(spacings.begin(), spacings.end());
        lineHeight = spacings[spacings.size() / 2]; // median
    }

    // Now detect character width using column density on a text band
    // Pick the densest text band
    int bestBand = 0;
    int bestBandDensity = 0;
    for (int i = 0; i < textBands.size(); ++i) {
        int h = textBands[i].second - textBands[i].first;
        if (qAbs(h - bestHeight) <= 2) {
            int density = 0;
            for (int y = textBands[i].first; y < textBands[i].second; ++y)
                density += rowDensity[y];
            if (density > bestBandDensity) {
                bestBandDensity = density;
                bestBand = i;
            }
        }
    }

    // Column density within that band
    int by0 = textBands[bestBand].first;
    int by1 = textBands[bestBand].second;
    QVector<int> colDensity(imgW, 0);
    for (int x = 0; x < imgW; ++x) {
        for (int y = by0; y < by1; ++y) {
            if (binary[y * imgW + x]) colDensity[x]++;
        }
    }

    // Find column gaps (empty columns between characters)
    // For fixed-width font, gaps appear at regular intervals
    QVector<int> gapPositions;
    for (int x = 1; x < imgW - 1; ++x) {
        if (colDensity[x] == 0 && (colDensity[x-1] > 0 || colDensity[x+1] > 0))
            gapPositions.append(x);
    }

    // Try common Nspire font widths: 6, 7, 8
    int bestWidth = 0;
    int bestWidthScore = 0;
    for (int tryWidth = 5; tryWidth <= 10; ++tryWidth) {
        int score = 0;
        // Check how many gap positions align with tryWidth grid
        for (int gap : gapPositions) {
            for (int offset = 0; offset < tryWidth; ++offset) {
                if ((gap - offset) % tryWidth == 0) {
                    score++;
                    break;
                }
            }
        }
        // Also check: does imgW / tryWidth give a reasonable char count?
        int chars = imgW / tryWidth;
        if (chars >= 30 && chars <= 60) score += 5;
        if (score > bestWidthScore) {
            bestWidthScore = score;
            bestWidth = tryWidth;
        }
    }

    // Fallback: try 6 (most common for Nspire)
    if (bestWidth == 0) bestWidth = 6;

    fm.charWidth = bestWidth;
    fm.charHeight = lineHeight;
    fm.startX = 0; // Will refine below
    fm.startY = textBands[0].first;
    fm.cols = imgW / bestWidth;
    fm.rows = (imgH - fm.startY) / lineHeight;
    fm.valid = true;

    // Refine startX: find the first column with any dark pixels
    for (int x = 0; x < imgW; ++x) {
        bool hasDark = false;
        for (int y = 0; y < imgH && !hasDark; ++y)
            hasDark = binary[y * imgW + x];
        if (hasDark) { fm.startX = x; break; }
    }
    // Align to grid
    fm.startX = (fm.startX / fm.charWidth) * fm.charWidth;
    fm.cols = (imgW - fm.startX) / fm.charWidth;

    return fm;
}

// ============ Helper: extract glyph bitmap for a character cell ============

static QByteArray extractGlyph(const QVector<bool> &binary, int imgW,
                                int cellX, int cellY, int cellW, int cellH)
{
    // Pack bits into bytes for compact hashing
    QByteArray glyph;
    glyph.reserve((cellW * cellH + 7) / 8);

    uint8_t byte = 0;
    int bit = 0;
    for (int y = cellY; y < cellY + cellH && y < 240; ++y) {
        for (int x = cellX; x < cellX + cellW && x < 320; ++x) {
            if (binary[y * imgW + x])
                byte |= (1 << bit);
            bit++;
            if (bit == 8) {
                glyph.append(static_cast<char>(byte));
                byte = 0;
                bit = 0;
            }
        }
    }
    if (bit > 0) glyph.append(static_cast<char>(byte));

    return glyph;
}

// ============ Tool: emulator_ocr_calibrate ============

QJsonObject MCPServer::toolEmulatorOcrCalibrate(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    // Option 1: Auto-calibrate from current screen (if reference text visible)
    // Option 2: Provide known text that's currently on screen
    QString knownText = args.value(QStringLiteral("known_text")).toString();

    if (knownText.isEmpty()) {
        return makeToolError(QStringLiteral(
            "Provide 'known_text' matching what's currently on the first visible "
            "text line of the screen. Include ALL characters exactly as displayed. "
            "Tip: type a known string into the REPL first, e.g. all printable ASCII."));
    }

    QImage image = renderFramebuffer();
    if (image.isNull()) {
        return makeToolError(QStringLiteral("Failed to capture framebuffer"));
    }

    // Convert to ARGB32 for reliable pixel access
    image = image.convertToFormat(QImage::Format_ARGB32);

    QVector<bool> binary = framebufferToBinary(image);
    FontMetrics fm = detectFontMetrics(binary, 320, 240);

    if (!fm.valid) {
        return makeToolError(QStringLiteral("Could not detect font metrics. Is there text on screen?"));
    }

    // Store metrics
    m_ocrCharWidth = fm.charWidth;
    m_ocrCharHeight = fm.charHeight;
    m_ocrBaselineX = fm.startX;
    m_ocrBaselineY = fm.startY;

    // Extract glyphs from first text line and map to known_text chars
    m_ocrGlyphTable.clear();
    int mapped = 0;

    for (int i = 0; i < knownText.length() && i < fm.cols; ++i) {
        char ch = knownText.at(i).toLatin1();
        if (ch == 0) continue;

        int cellX = fm.startX + i * fm.charWidth;
        int cellY = fm.startY;

        QByteArray glyph = extractGlyph(binary, 320, cellX, cellY, fm.charWidth, fm.charHeight);

        // Skip empty glyphs (spaces)
        bool allEmpty = true;
        for (char b : glyph) { if (b != 0) { allEmpty = false; break; } }

        if (ch == ' ') {
            // Map the empty glyph to space
            m_ocrGlyphTable[glyph] = ' ';
            mapped++;
        } else if (!allEmpty) {
            m_ocrGlyphTable[glyph] = ch;
            mapped++;
        }
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("char_width")] = fm.charWidth;
    result[QStringLiteral("char_height")] = fm.charHeight;
    result[QStringLiteral("start_x")] = fm.startX;
    result[QStringLiteral("start_y")] = fm.startY;
    result[QStringLiteral("cols")] = fm.cols;
    result[QStringLiteral("rows")] = fm.rows;
    result[QStringLiteral("glyphs_mapped")] = mapped;
    result[QStringLiteral("total_unique_glyphs")] = m_ocrGlyphTable.size();
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ Tool: emulator_ocr ============

QJsonObject MCPServer::toolEmulatorOcr(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QImage image = renderFramebuffer();
    if (image.isNull()) {
        return makeToolError(QStringLiteral("Failed to capture framebuffer"));
    }

    image = image.convertToFormat(QImage::Format_ARGB32);
    QVector<bool> binary = framebufferToBinary(image);

    int charW, charH, startX, startY;

    if (m_ocrCharWidth > 0) {
        // Use calibrated metrics
        charW = m_ocrCharWidth;
        charH = m_ocrCharHeight;
        startX = m_ocrBaselineX;
        startY = m_ocrBaselineY;
    } else {
        // Auto-detect
        FontMetrics fm = detectFontMetrics(binary, 320, 240);
        if (!fm.valid) {
            return makeToolError(QStringLiteral(
                "Could not detect text. Run emulator_ocr_calibrate first, "
                "or ensure there is text on screen."));
        }
        charW = fm.charWidth;
        charH = fm.charHeight;
        startX = fm.startX;
        startY = fm.startY;
    }

    int cols = (320 - startX) / charW;
    int rows = (240 - startY) / charH;

    // Optional: extract specific region
    int regionTop = args.value(QStringLiteral("line_start")).toInt(0);
    int regionLines = args.value(QStringLiteral("line_count")).toInt(rows);
    if (regionTop + regionLines > rows) regionLines = rows - regionTop;

    QStringList lines;
    int unknownCount = 0;

    for (int row = regionTop; row < regionTop + regionLines; ++row) {
        QString line;
        int cellY = startY + row * charH;

        for (int col = 0; col < cols; ++col) {
            int cellX = startX + col * charW;

            QByteArray glyph = extractGlyph(binary, 320, cellX, cellY, charW, charH);

            // Check if glyph is empty (whitespace)
            bool allEmpty = true;
            for (char b : glyph) { if (b != 0) { allEmpty = false; break; } }

            if (allEmpty) {
                line += QLatin1Char(' ');
            } else if (m_ocrGlyphTable.contains(glyph)) {
                line += QLatin1Char(m_ocrGlyphTable[glyph]);
            } else {
                line += QLatin1Char('?');
                unknownCount++;
            }
        }

        // Trim trailing spaces
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        lines.append(line);
    }

    // Trim trailing empty lines
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("text")] = lines.join(QStringLiteral("\n"));
    result[QStringLiteral("lines")] = lines.size();
    result[QStringLiteral("cols")] = cols;
    result[QStringLiteral("char_size")] = QStringLiteral("%1x%2").arg(charW).arg(charH);
    if (unknownCount > 0) {
        result[QStringLiteral("unknown_glyphs")] = unknownCount;
        result[QStringLiteral("hint")] = QStringLiteral("Run emulator_ocr_calibrate with known text to improve recognition");
    }
    result[QStringLiteral("calibrated")] = (m_ocrCharWidth > 0);
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

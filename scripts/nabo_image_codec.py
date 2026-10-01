"""Lossless LVGL-native image encoding without an external compression package."""


def rgb565a8(image):
    colors, alpha = bytearray(), bytearray()
    pixels = image.convert('RGBA').tobytes()
    for offset in range(0, len(pixels), 4):
        r, g, b, a = pixels[offset:offset + 4]
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        colors.extend(value.to_bytes(2, 'little'))
        alpha.append(a)
    return bytes(colors + alpha)


def lz4_block(data):
    """Encode a standard raw LZ4 block; leave its mandatory final literals.

    A greedy four-byte dictionary is sufficient for image planes. Decoder
    compatibility and byte-for-byte output are verified against LVGL's LZ4.
    """
    output, seen = bytearray(), {}
    anchor = position = 0
    size = len(data)

    def extension(length):
        while length >= 255:
            output.append(255)
            length -= 255
        output.append(length)

    while position <= size - 12:
        key = data[position:position + 4]
        previous = seen.get(key)
        seen[key] = position
        if previous is None or position - previous > 65535:
            position += 1
            continue
        length = 4
        while position + length < size - 5 and data[previous + length] == data[position + length]:
            length += 1
        literals = position - anchor
        output.append((min(literals, 15) << 4) | min(length - 4, 15))
        if literals >= 15:
            extension(literals - 15)
        output.extend(data[anchor:position])
        output.extend((position - previous).to_bytes(2, 'little'))
        if length - 4 >= 15:
            extension(length - 19)
        end = position + length
        for index in range(position + 1, min(end, size - 3)):
            seen[data[index:index + 4]] = index
        position = anchor = end
    literals = size - anchor
    output.append(min(literals, 15) << 4)
    if literals >= 15:
        extension(literals - 15)
    output.extend(data[anchor:])
    return bytes(output)

#version 330 core
// Depth-only pass: the fragment shader writes nothing of its own; the
// fixed-function depth buffer captures the distance to the sun. An empty
// main keeps the program valid on all 3.3 core drivers.
void main() {
}

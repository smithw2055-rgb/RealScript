#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "tests/phase18_native_control_flow_tests.cpp"
text = path.read_text(encoding="utf-8")
old = '''void testNativeValueAliases() {
    const auto result = execute(R"(
module Phase18;
int main()
{
'''
new = '''void testNativeValueAliases() {
    const auto result = execute(R"(
module Phase18;
double main()
{
'''
if new not in text:
    if old not in text:
        raise RuntimeError("native alias test anchor not found")
    text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")

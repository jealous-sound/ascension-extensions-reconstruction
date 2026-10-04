import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from native_ca_packets import function


ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <Ascension/AscCallbackOrder.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <vector>
#define __cdecl
#define __fastcall
namespace AscRuntime
{
using UnitSpellVeto = bool (*)(void*, uint32_t, uint32_t);
using CastVeto = bool (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
std::vector<std::pair<uint32_t, UnitSpellVeto>>& Before71E930List()
{ static std::vector<std::pair<uint32_t, UnitSpellVeto>> v; return v; }
std::vector<std::pair<uint32_t, CastVeto>>& After80B5D0List()
{ static std::vector<std::pair<uint32_t, CastVeto>> v; return v; }
@INSERT@
@REMOVE_REGISTER@
@CAST_REGISTER@
std::vector<int> trace;
int veto = -1;
bool clientCast = true;
int failures = 0, checks = 0;
void Check(bool result)
{ ++checks; failures += !result; }
template<int N> bool Aura(void* unit, uint32_t a, uint32_t b)
{
    Check(unit == reinterpret_cast<void*>(123) && a == 4 && b == 5);
    trace.push_back(N);
    return veto != N;
}
template<int N> bool Cast(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    Check(a == 1 && b == 2 && c == 3 && d == 4 && e == 5);
    trace.push_back(N);
    return veto != N;
}
int OriginalRemove(void*, void*, uint32_t, uint32_t) { trace.push_back(9); return 7; }
int OriginalCast(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{ trace.push_back(9); return clientCast; }
auto g_71E930Trampoline = &OriginalRemove;
auto g_80B5D0Trampoline = &OriginalCast;
@REMOVE_DETOUR@
@CAST_DETOUR@
}
int main()
{
    using namespace AscRuntime;
    const uint32_t auraSites[] = {@AURA_SITES@};
    const uint32_t castSites[] = {@CAST_SITES@};
    UnitSpellVeto aura[] = {&Aura<0>, &Aura<1>};
    CastVeto cast[] = {&Cast<0>, &Cast<1>, &Cast<2>};
    std::array<int, 2> auraOrder{0, 1};
    do
    {
        Before71E930List().clear();
        for (int i : auraOrder) OnBefore71E930(aura[i], auraSites[i]);
        for (veto = -1; veto < 2; ++veto)
        {
            trace.clear();
            int result = Detour71E930(reinterpret_cast<void*>(123), nullptr, 4, 5);
            std::vector<int> expected = veto < 0 ? std::vector<int>{0, 1, 9} :
                veto == 0 ? std::vector<int>{0} : std::vector<int>{0, 1};
            Check(trace == expected && result == (veto < 0 ? 7 : 0));
        }
        OnBefore71E930(aura[0], auraSites[0]);
        Check(Before71E930List().size() == 2);
    } while (std::next_permutation(auraOrder.begin(), auraOrder.end()));
    std::array<int, 3> castOrder{0, 1, 2};
    do
    {
        After80B5D0List().clear();
        for (int i : castOrder) OnAfter80B5D0(cast[i], castSites[i]);
        for (veto = -1; veto < 3; ++veto)
        {
            trace.clear();
            int result = Detour80B5D0(1, 2, 3, 4, 5);
            std::vector<int> expected{9};
            for (int i = 0; i < (veto < 0 ? 3 : veto + 1); ++i) expected.push_back(i);
            Check(trace == expected && result == (veto < 0 ? 1 : 0));
        }
        clientCast = false;
        trace.clear();
        Check(Detour80B5D0(1, 2, 3, 4, 5) == 0 && trace == std::vector<int>{9});
        clientCast = true;
        OnAfter80B5D0(cast[0], castSites[0]);
        Check(After80B5D0List().size() == 3);
    } while (std::next_permutation(castOrder.begin(), castOrder.end()));
    std::cout << checks << " checks, " << failures << " failures: original callback order, vetoes, "
              << "client fall-through and duplicate registration\n";
    return failures ? 1 : 0;
}
'''


def check_relocation_buckets(fixture):
    def bucket(address, base):
        pointer = (int(address, 16) - int(fixture['image_base'], 16) + base) & 0xffffffff
        hashed = 0x811c9dc5
        for byte in pointer.to_bytes(4, 'little'):
            hashed = ((hashed ^ byte) * 0x1000193) & 0xffffffff
        return hashed & fixture['bucket_mask']

    for base in range(0, 0x100000000, 0x10000):
        aura = [bucket(e['function'], base) for e in fixture['Before71E930']['callbacks']]
        cast = [bucket(e['function'], base) for e in fixture['After80B5D0']['callbacks']]
        assert aura[0] != aura[1] and cast[0] == cast[1] and cast[0] != cast[2], hex(base)
    print('PASS: native bucket relationships survive all 65,536 allocation-aligned 32-bit image bases', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Compare production registrars and detours with native DLL order.')
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    name = 'src/Ascension/AscRuntime.cpp'
    source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{name}'], cwd=ROOT, text=True) \
        if args.source_ref else (ROOT / name).read_text(encoding='utf-8')
    fixture = json.loads((ROOT / 'tests/fixtures/original_callback_order.json').read_text(encoding='utf-8'))
    check_relocation_buckets(fixture)
    harness = HARNESS
    for marker, signature in [
        ('INSERT', 'template <class T> void InsertBySite('),
        ('REMOVE_REGISTER', 'void OnBefore71E930('),
        ('CAST_REGISTER', 'void OnAfter80B5D0('),
        ('REMOVE_DETOUR', 'int __fastcall Detour71E930('),
        ('CAST_DETOUR', 'int __cdecl Detour80B5D0('),
    ]:
        harness = harness.replace(f'@{marker}@', function(source, signature))
    for marker, name in [('AURA_SITES', 'Before71E930'), ('CAST_SITES', 'After80B5D0')]:
        harness = harness.replace(f'@{marker}@', ', '.join(e['site'] for e in fixture[name]['callbacks']))
    compiler = shutil.which(os.environ.get('CXX', 'cl.exe' if os.name == 'nt' else 'c++'))
    if not compiler:
        raise SystemExit('A C++20 compiler is required.')
    with tempfile.TemporaryDirectory(prefix='native-callback-order-') as directory:
        out = Path(directory)
        cpp = out / 'callbacks.cpp'
        cpp.write_text(harness, encoding='utf-8')
        executable = out / ('callbacks.exe' if os.name == 'nt' else 'callbacks')
        if Path(compiler).stem.lower() == 'cl':
            flags = ['/nologo', '/std:c++20', '/EHsc', '/I' + str(ROOT / 'src'),
                     str(cpp), '/Fe' + str(executable)]
        else:
            flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                     str(cpp), '-o', str(executable)]
        subprocess.run([compiler, *flags], cwd=out, check=True)
        return subprocess.run([str(executable)], cwd=out).returncode


if __name__ == '__main__':
    raise SystemExit(main())

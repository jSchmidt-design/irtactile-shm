// Polyfill for netstandard2.0: the compiler needs this type for `init`
// accessors, which `record struct` positional parameters generate. .NET 5+
// ships it in the BCL, so the polyfill exists only where it is missing.
//
// SPDX-License-Identifier: MIT

#if !NET5_0_OR_GREATER
namespace System.Runtime.CompilerServices;

internal static class IsExternalInit { }
#endif

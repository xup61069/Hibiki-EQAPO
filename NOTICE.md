# Loudness-profile notice

Hibiki EQAPO is the product name of this compatibility fork. It is not the upstream Equalizer APO project or an official upstream build; upstream names below are retained solely for accurate attribution and compatibility documentation.

This repository includes a formula-driven loudness-correction implementation and tabular parameter set, plus a separately named implementation of the original Mixomo two-shelf loudness-correction algorithm derived from this fork's declared upstream source lineage.

The original component preserves the algorithm found in Mixomo's initial public import, commit [`3a0cc87`](https://github.com/Mixomo/EqAPO64_with_VST3_support/commit/3a0cc87e1dc73c71158d178729f30dbe679872a9), whose loudness-correction source identifies Alexander Walch as its 2017 copyright holder. The implementation in this repository retains that attribution while replacing unsafe lifecycle and real-time behavior.

The repository owner has confirmed permission to redistribute these included materials publicly in source and binary form. The permission applies to this repository and its released installer packages.

That confirmation covers only the loudness-profile table and its implementation. It does not cover third-party headphone-measurement catalogs or impulse-response audio. The public source history and installer intentionally exclude those datasets; users and private builders must supply only data that they are licensed to use.

This project is presented as a loudness-correction implementation only. It makes no claim of standards conformance, certification, endorsement, affiliation, or approval.

Hibiki EQAPO is compatible with ASIO® technology. Its x64 proxy is built against the official Steinberg ASIO SDK 2.3.4 headers through the repository's pinned vcpkg manifest and uses the SDK's GPLv3 licensing path. The corresponding project source, dependency manifest, and build scripts are distributed with this repository. This does not imply Steinberg endorsement or affiliation.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

The program code remains subject to the GPL-3.0 license in [LICENSE](LICENSE). This notice accompanies the source repository and installed binary package.

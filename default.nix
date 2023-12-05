with import <nixpkgs> {};

sniproxy.overrideAttrs(old: {
  src = ./.;
})


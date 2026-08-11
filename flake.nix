{
  description = "Bonded Inbox Logos Core module";

  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [
      "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU="
    ];
  };

  inputs = {
    logos-module-builder.url =
      "github:logos-co/logos-module-builder/7fbb9420a3fe8ce03a140f0df84a0cc8463bc6dc";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      core = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };
      basecamp = logos-module-builder.lib.mkLogosQmlModule {
        src = ./basecamp;
        configFile = ./basecamp/metadata.json;
        flakeInputs = inputs;
      };
    in
    core // {
      packages = builtins.mapAttrs
        (system: packages:
          packages // {
            basecamp = basecamp.packages.${system}.default;
            basecamp-lgx = basecamp.packages.${system}.lgx;
          })
        core.packages;
    };
}

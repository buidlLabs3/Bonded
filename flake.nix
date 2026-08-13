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
    delivery_module = {
      url = "github:logos-co/logos-delivery-module/3f0f2d8b202f427a96179407bbf18b449935da7c";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };
    storage_module = {
      url = "github:logos-co/logos-storage-module/e0db835de379f47bae7fccc3032056d99af973bb";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };
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
